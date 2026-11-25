// Tray application: hidden window, timers, device/power notifications, the
// background poll thread and the context menu.
#include "app.h"

#include <windows.h>
#include <windowsx.h>
#include <dbt.h>
#include <shellapi.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "../../assets/resource.h"
#include "../core/device_manager.h"
#include "../core/log.h"
#include "../core/settings.h"
#include "../core/updater.h"
#include "icon_renderer.h"
#include "settings_window.h"
#include "taskbar_widget.h"
#include "tray.h"

namespace bh {
namespace {

constexpr wchar_t kWindowClass[] = L"BatteryHub.TrayWindow";
constexpr UINT WM_APP_TRAY = WM_APP + 1;
constexpr UINT WM_APP_POLL_DONE = WM_APP + 2;
constexpr UINT WM_APP_WAKE = WM_APP + 3;  // sent by a second instance
constexpr UINT WM_APP_WIDGET_MENU = WM_APP + 4;  // click on the taskbar widget (x, y)
constexpr UINT WM_APP_REORDER = WM_APP + 5;      // user dragged a widget item to a new place
constexpr UINT WM_APP_UPDATE_CHECKED = WM_APP + 6;  // lParam: UpdateInfo*, wParam: 1 = asked by the user
constexpr UINT WM_APP_UPDATE_DONE = WM_APP + 7;     // wParam: success, lParam: std::wstring* error

constexpr UINT_PTR TIMER_POLL = 1;
constexpr UINT_PTR TIMER_DEBOUNCE = 2;
constexpr UINT_PTR TIMER_WIDGET = 3;  // keeps the widget anchored next to the clock
constexpr UINT_PTR TIMER_UPDATE = 4;  // background update check (30 s after start, then daily)

enum : UINT {
    ID_DEVICE_FIRST = 1000,  // + device index: rename
    ID_REFRESH = 2000,
    ID_SETTINGS,
    ID_EXIT,
    ID_UPDATE_CHECK = 2300,
    ID_UPDATE_INSTALL = 2301,
};

// GUIDs for interface arrival notifications.
const GUID kHidInterface = {0x4D1E55B2, 0xF16F, 0x11CF, {0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};
const GUID kBthDevice = {0x0850302A, 0xB344, 0x4FDA, {0x9B, 0xE9, 0x90, 0x57, 0x6B, 0x8D, 0x46, 0xF0}};
const GUID kBleDevice = {0x781AEE18, 0x7733, 0x4CE4, {0xAD, 0xD0, 0x91, 0xF4, 0x1C, 0x67, 0xB5, 0x92}};

struct AppState {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    UINT taskbarCreated = 0;
    Settings settings;
    DeviceManager devices;
    Tray tray;
    TaskbarWidget widget;
    HANDLE worker = nullptr;
    bool polling = false;
    bool pollAgain = false;
    bool gaming = false;  // full-screen game in front: hidden, not polling
    int gameTicks = 0;    // consecutive 1 s checks that disagree with `gaming`
    std::vector<HDEVNOTIFY> notifications;
    std::vector<std::wstring> menuIds;  // device id per menu entry
    UpdateInfo update;                  // result of the last update check
    bool updateChecking = false, updateInstalling = false;
    bool updateChecked = false;         // at least one check finished (for the settings window)
};
AppState* g = nullptr;

std::wstring DisplayName(const BatteryReading& r) {
    std::wstring alias = GetAlias(r.id);
    if (!alias.empty()) return alias;
    return r.name.empty() ? std::wstring(KindName(r.kind)) : r.name;
}

int TrayIconSize() {
    HDC dc = GetDC(nullptr);
    int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    using Fn = int(WINAPI*)(int, UINT);
    static Fn getMetricsForDpi =
        (Fn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi");
    if (getMetricsForDpi) return getMetricsForDpi(SM_CXSMICON, (UINT)dpi);
    return MulDiv(16, dpi, 96);
}

// Shown only while the device (or its dongle) is present. A present device that
// does not answer (asleep) keeps its last value, dimmed; an unplugged or switched-off
// one (Bluetooth pad turned off, cable removed, dongle pulled) disappears.
bool IsVisible(const TrackedDevice& d) {
    if (!d.present) return false;
    if (g->settings.hideDisconnected && !d.r.online) return false;
    return d.r.percent >= 0 || d.r.online;
}

// Exclusive full-screen (D3D), "busy" full-screen apps, or a foreground window
// covering its whole monitor (borderless-windowed games).
bool IsBrowserWindow(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return false;
    wchar_t path[MAX_PATH] = {};
    DWORD len = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(proc, 0, path, &len) != 0;
    CloseHandle(proc);
    if (!ok) return false;
    const wchar_t* exe = wcsrchr(path, L'\\');
    exe = exe ? exe + 1 : path;
    for (const wchar_t* b : {L"brave.exe", L"chrome.exe", L"msedge.exe", L"firefox.exe", L"opera.exe",
                             L"opera_gx.exe", L"vivaldi.exe", L"arc.exe", L"zen.exe", L"librewolf.exe"})
        if (!_wcsicmp(exe, b)) return true;
    return false;
}

bool IsFullscreenAppInFront() {
    // A browser in full screen is usually a video, not a game: ignored unless enabled.
    HWND front = GetForegroundWindow();
    if (front && !g->settings.gameModeBrowsers && IsBrowserWindow(front)) return false;
    QUERY_USER_NOTIFICATION_STATE st;
    bool queried = SUCCEEDED(SHQueryUserNotificationState(&st));
    if (queried && st == QUNS_RUNNING_D3D_FULL_SCREEN) return true;  // exclusive full screen
    HWND fg = front;
    if (!fg || fg == g->hwnd) return false;
    // A normal maximized window is not a game, even when it covers the whole monitor
    // (auto-hide taskbar, TranslucentTB setups...): it has a title bar. Borderless games
    // have none, whether or not Windows considers them maximized.
    LONG style = GetWindowLongW(fg, GWL_STYLE);
    if ((style & WS_CAPTION) == WS_CAPTION) return false;
    if (queried && (st == QUNS_BUSY || st == QUNS_PRESENTATION_MODE)) return true;
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, 64);
    for (const wchar_t* shell : {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"})
        if (!wcscmp(cls, shell)) return false;
    RECT wr;
    MONITORINFO mi = {sizeof(mi)};
    if (!GetWindowRect(fg, &wr) || !GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi))
        return false;
    return wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top && wr.right >= mi.rcMonitor.right &&
           wr.bottom >= mi.rcMonitor.bottom;
}

int OrderRank(const std::wstring& id) {
    const auto& o = g->settings.order;
    for (size_t i = 0; i < o.size(); ++i)
        if (o[i] == id) return (int)i;
    return 1 << 20;
}

// Visible devices in the user's order (the taskbar, the menu and the settings window agree).
std::vector<const TrackedDevice*> ShownDevices() {
    std::vector<const TrackedDevice*> shown;
    for (const auto& d : g->devices.Devices())
        if (IsVisible(d)) shown.push_back(&d);
    std::stable_sort(shown.begin(), shown.end(), [](const TrackedDevice* a, const TrackedDevice* b) {
        return OrderRank(a->r.id) < OrderRank(b->r.id);
    });
    return shown;
}

void UpdateTray() {
    if (g->gaming) return;  // game mode: stay hidden until the game is gone
    const bool light = IsTaskbarLight();
    const int size = TrayIconSize();
    std::vector<TrayItem> items;
    for (const auto& d : g->devices.Devices()) {
        if (!IsVisible(d)) continue;
        TrayItem it;
        it.id = d.r.id;
        it.spec.kind = d.r.kind;
        it.spec.percent = d.r.percent;
        it.spec.charge = d.r.online ? d.r.charge : Charge::Unknown;
        it.spec.stale = d.stale;
        it.spec.low = d.r.percent >= 0 && d.r.percent <= g->settings.lowThreshold &&
                      it.spec.charge != Charge::Charging;
        it.spec.light = light;
        it.spec.size = size;
        std::wstring pct = d.r.percent >= 0 ? Fmt(L"%d%%", d.r.percent) : std::wstring(L"--");
        it.tip = DisplayName(d.r) + L"\n" + pct + L" · " + ConnName(d.r.conn);
        if (d.r.online && d.r.charge != Charge::Unknown && d.r.charge != Charge::Discharging)
            it.tip += std::wstring(L" · ") + ChargeName(d.r.charge);
        if (d.r.online && d.r.percent < 0 && d.r.charge != Charge::Charging)
            it.tip += L"\nConectado, pero no informa su batería";
        else if (d.stale)
            it.tip += L"\nÚltimo valor conocido (en reposo, no responde)";
        items.push_back(it);
    }
    // User-defined order (drag & drop); devices never dragged keep arrival order at the end.
    std::stable_sort(items.begin(), items.end(),
                     [](const TrayItem& a, const TrayItem& b) { return OrderRank(a.id) < OrderRank(b.id); });

    // Several controllers: number them 1, 2, 3... (glyph badge + tooltip).
    int pads = 0;
    for (const auto& it : items) pads += it.spec.kind == DeviceKind::Gamepad;
    if (pads > 1) {
        int n = 0;
        for (auto& it : items)
            if (it.spec.kind == DeviceKind::Gamepad) {
                it.spec.badge = ++n;
                it.tip = Fmt(L"Mando %d: ", n) + it.tip;
            }
    }
    if (items.empty()) {
        TrayItem it;
        it.id = L"__none__";
        it.spec.placeholder = true;
        it.spec.light = light;
        it.spec.size = size;
        it.spec.stale = true;
        it.tip = L"BatteryHub\nNo se detectaron dispositivos con batería";
        items.push_back(it);
    }

    // Inline taskbar display unless the user chose tray icons or the taskbar
    // layout does not allow it (vertical taskbar, unknown shell) -> tray fallback.
    if (!g->settings.trayIcons) {
        if (!g->widget.Ok()) g->widget.Create(g->inst, g->hwnd, WM_APP_WIDGET_MENU, WM_APP_REORDER);
        if (g->widget.Ok()) {
            g->widget.SetAnchorLeft(g->settings.widgetLeft);
            std::vector<WidgetItem> w;
            for (const auto& it : items) w.push_back({it.id, it.spec, it.tip});
            g->widget.Update(w);
            if (g->widget.Reposition()) {
                g->tray.RemoveAll();
                return;
            }
            g->widget.Destroy();
        }
    } else if (g->widget.Ok()) {
        g->widget.Destroy();
    }
    g->tray.Sync(items);
}

DWORD WINAPI PollThread(void*) {
    auto* result = new std::vector<BatteryReading>(g->devices.PollAll());
    if (!PostMessageW(g->hwnd, WM_APP_POLL_DONE, 0, (LPARAM)result)) delete result;
    return 0;
}

void StartPoll() {
    if (g->gaming) return;  // no HID traffic while playing; polled again on exit
    if (g->polling) {
        g->pollAgain = true;
        return;
    }
    if (g->worker) {
        CloseHandle(g->worker);
        g->worker = nullptr;
    }
    g->polling = true;
    g->worker = CreateThread(nullptr, 0, PollThread, nullptr, 0, nullptr);
    if (!g->worker) g->polling = false;
}

void RestartPollTimer() { SetTimer(g->hwnd, TIMER_POLL, (UINT)g->settings.intervalSec * 1000, nullptr); }

void Debounce(UINT ms) { SetTimer(g->hwnd, TIMER_DEBOUNCE, ms, nullptr); }

// ---------------------------------------------------------------- rename dialog
struct RenameCtx {
    std::wstring id, original, current;
};

INT_PTR CALLBACK RenameProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = (RenameCtx*)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg) {
        case WM_INITDIALOG:
            ctx = (RenameCtx*)lp;
            SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)ctx);
            SetDlgItemTextW(dlg, IDC_ORIGINAL, (L"Nombre original: " + ctx->original).c_str());
            SetDlgItemTextW(dlg, IDC_ALIAS, ctx->current.c_str());
            SendDlgItemMessageW(dlg, IDC_ALIAS, EM_SETLIMITTEXT, 60, 0);
            SendDlgItemMessageW(dlg, IDC_ALIAS, EM_SETSEL, 0, -1);
            SetForegroundWindow(dlg);
            return TRUE;
        case WM_COMMAND:
            if (LOWORD(wp) == IDOK) {
                wchar_t buf[64] = {};
                GetDlgItemTextW(dlg, IDC_ALIAS, buf, 64);
                std::wstring alias = buf;
                while (!alias.empty() && alias.back() == L' ') alias.pop_back();
                if (alias == ctx->original) alias.clear();
                SetAlias(ctx->id, alias);
                EndDialog(dlg, IDOK);
                return TRUE;
            }
            if (LOWORD(wp) == IDCANCEL) {
                EndDialog(dlg, IDCANCEL);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

void Rename(const std::wstring& id) {
    for (const auto& d : g->devices.Devices()) {
        if (d.r.id != id) continue;
        RenameCtx ctx{id, d.r.name.empty() ? KindName(d.r.kind) : d.r.name, DisplayName(d.r)};
        // Over the settings window when it is open, so it stays modal to it.
        HWND owner = SettingsWindowHandle() ? SettingsWindowHandle() : g->hwnd;
        if (DialogBoxParamW(g->inst, MAKEINTRESOURCEW(IDD_RENAME), owner, RenameProc, (LPARAM)&ctx) == IDOK) {
            UpdateTray();
            RefreshSettingsWindow();
        }
        return;
    }
}

// Moves a device one place earlier/later among the visible ones.
void MoveDevice(const std::wstring& id, int delta) {
    std::vector<std::wstring> ids;
    for (const TrackedDevice* d : ShownDevices()) ids.push_back(d->r.id);
    auto it = std::find(ids.begin(), ids.end(), id);
    if (it == ids.end()) return;
    size_t i = (size_t)(it - ids.begin());
    if ((delta < 0 && i == 0) || (delta > 0 && i + 1 >= ids.size())) return;
    std::swap(ids[i], ids[delta < 0 ? i - 1 : i + 1]);
    for (const auto& old : g->settings.order)  // remembered devices not shown right now keep their place at the end
        if (std::find(ids.begin(), ids.end(), old) == ids.end()) ids.push_back(old);
    g->settings.order = ids;
    g->settings.Save();
    UpdateTray();
}

// ---------------------------------------------------------------- context menu
// ---------------------------------------------------------------- updates
struct CheckJob {
    HWND hwnd;
    bool manual;
};

DWORD WINAPI UpdateCheckThread(void* p) {
    CheckJob job = *(CheckJob*)p;
    delete (CheckJob*)p;
    auto* info = new UpdateInfo(CheckForUpdate());
    if (!PostMessageW(job.hwnd, WM_APP_UPDATE_CHECKED, job.manual ? 1 : 0, (LPARAM)info)) delete info;
    return 0;
}

void StartUpdateCheck(bool manual) {
    if (g->updateChecking || g->updateInstalling) return;
    g->updateChecking = true;
    HANDLE t = CreateThread(nullptr, 0, UpdateCheckThread, new CheckJob{g->hwnd, manual}, 0, nullptr);
    if (t) CloseHandle(t);
    else g->updateChecking = false;
}

DWORD WINAPI UpdateInstallThread(void* p) {
    HWND hwnd = (HWND)p;
    UpdateInfo info = CheckForUpdate();  // fresh URL, in case a newer release appeared meanwhile
    auto* err = new std::wstring;
    bool ok = info.ok && info.newer && InstallUpdate(info, err);
    if (info.ok && !info.newer) *err = L"ya tenés la última versión";
    else if (!info.ok) *err = info.error;
    if (!PostMessageW(hwnd, WM_APP_UPDATE_DONE, ok ? 1 : 0, (LPARAM)err)) delete err;
    return 0;
}

void StartUpdateInstall() {
    if (g->updateInstalling) return;
    g->updateInstalling = true;
    HANDLE t = CreateThread(nullptr, 0, UpdateInstallThread, g->hwnd, 0, nullptr);
    if (t) CloseHandle(t);
    else g->updateInstalling = false;
}

void OpenReleasesPage() { ShellExecuteW(nullptr, L"open", kReleasesPage, nullptr, nullptr, SW_SHOWNORMAL); }

// ---------------------------------------------------------------- settings window
class AppSettingsHost : public SettingsHost {
public:
    Settings& Prefs() override { return g->settings; }

    std::vector<DeviceView> Devices() override {
        std::vector<DeviceView> out;
        for (const TrackedDevice* dp : ShownDevices()) {
            const TrackedDevice& d = *dp;
            DeviceView v;
            v.id = d.r.id;
            v.name = DisplayName(d.r);
            v.kind = d.r.kind;
            v.percent = d.r.percent;
            v.stale = d.stale;
            v.charging = d.r.online && d.r.charge == Charge::Charging;
            v.detail = ConnName(d.r.conn);
            if (d.r.online && (d.r.charge == Charge::Charging || d.r.charge == Charge::Full))
                v.detail += std::wstring(L" · ") + ChargeName(d.r.charge);
            if (d.r.online && d.r.percent < 0 && d.r.charge != Charge::Charging)
                v.detail += L" · no informa su batería";
            else if (d.stale)
                v.detail += L" · en reposo";
            out.push_back(v);
        }
        return out;
    }

    void Changed(Change what) override {
        g->settings.Save();
        if (what == Interval) RestartPollTimer();
        if (what == Display) UpdateTray();
        if (what == Display) RefreshSettingsWindow();  // "hide asleep" changes the list
    }
    void Rename(const std::wstring& id) override { bh::Rename(id); }
    void Move(const std::wstring& id, int delta) override { MoveDevice(id, delta); }
    void RefreshNow() override { StartPoll(); }
    bool Autostart() override { return IsAutostartEnabled(); }
    void SetAutostart(bool on) override {
        if (!bh::SetAutostart(on))
            MessageBoxW(SettingsWindowHandle(), L"No se pudo modificar el inicio automático.", L"BatteryHub",
                        MB_ICONWARNING);
    }
    UpdateView Update() override {
        UpdateView v;
        v.latest = g->update.latest;
        if (g->updateInstalling) v.state = UpdateView::Installing;
        else if (g->updateChecking) v.state = UpdateView::Checking;
        else if (g->update.ok && g->update.newer) v.state = UpdateView::Available;
        else if (g->update.ok) v.state = UpdateView::UpToDate;
        else if (g->updateChecked) v.state = UpdateView::Failed;
        return v;
    }
    void CheckUpdate() override {
        StartUpdateCheck(false);  // quiet: the window shows the result itself
        RefreshSettingsWindow();
    }
    void InstallUpdate() override {
        StartUpdateInstall();
        RefreshSettingsWindow();
    }
};
AppSettingsHost g_settingsHost;

void OpenSettings() { OpenSettingsWindow(g->inst, &g_settingsHost); }

// Dark/light popup menus. uxtheme's SetPreferredAppMode (ordinal 135, Windows 10
// 1903+) is undocumented but what Explorer and most tray apps use; FlushMenuThemes
// (ordinal 136) makes the change apply to the next menu. No-op on older systems.
bool MenuIsDark() {
    if (g->settings.menuTheme == 1) return true;
    if (g->settings.menuTheme == 2) return false;
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

void ApplyMenuTheme(bool dark) {
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetModeFn = int(WINAPI*)(int);
    using FlushFn = void(WINAPI*)();
    static auto setMode = (SetModeFn)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    static auto flush = (FlushFn)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (setMode) setMode(dark ? 2 /*ForceDark*/ : 3 /*ForceLight*/);
    if (flush) flush();
}

// Builds menus with an icon-font glyph per item; owns the icon bitmaps.
class MenuBuilder {
public:
    explicit MenuBuilder(bool dark) : dark_(dark) {
        HDC dc = GetDC(nullptr);
        size_ = MulDiv(16, GetDeviceCaps(dc, LOGPIXELSX), 96);
        ReleaseDC(nullptr, dc);
    }
    ~MenuBuilder() {
        for (HBITMAP b : bitmaps_) DeleteObject(b);
    }
    void Item(HMENU m, UINT flags, UINT_PTR id, const std::wstring& text, const wchar_t* glyph = nullptr) {
        AppendMenuW(m, flags, id, text.c_str());
        if (!glyph) return;
        HBITMAP b = RenderMenuGlyph(glyph, size_, dark_);
        if (!b) return;
        bitmaps_.push_back(b);
        MENUITEMINFOW mi = {sizeof(mi)};
        mi.fMask = MIIM_BITMAP;
        mi.hbmpItem = b;
        SetMenuItemInfoW(m, GetMenuItemCount(m) - 1, TRUE, &mi);
    }
    void Separator(HMENU m) { AppendMenuW(m, MF_SEPARATOR, 0, nullptr); }

private:
    bool dark_;
    int size_ = 16;
    std::vector<HBITMAP> bitmaps_;
};

// Icon-font glyphs (Segoe Fluent Icons / MDL2 Assets).
constexpr const wchar_t* kGlyphRefresh = L"\uE72C";
constexpr const wchar_t* kGlyphSettings = L"\uE713";
constexpr const wchar_t* kGlyphExit = L"\uE7E8";
constexpr const wchar_t* kGlyphDisplay = L"\uE7F4";
constexpr const wchar_t* kGlyphTimer = L"\uE916";
constexpr const wchar_t* kGlyphBatteryLow = L"\uE850";
constexpr const wchar_t* kGlyphTheme = L"\uE790";
constexpr const wchar_t* kGlyphInfo = L"\uE946";
constexpr const wchar_t* kGlyphDownload = L"\uE896";
constexpr const wchar_t* kGlyphCheck = L"\uE73E";
constexpr const wchar_t* kGlyphSync = L"\uE895";

void ShowMenu(POINT pt) {
    const bool dark = MenuIsDark();
    ApplyMenuTheme(dark);
    MenuBuilder mb(dark);
    HMENU menu = CreatePopupMenu();
    g->menuIds.clear();

    // Devices, in the same order as the taskbar.
    UINT idx = 0;
    for (const TrackedDevice* dp : ShownDevices()) {
        const TrackedDevice& d = *dp;
        std::wstring pct = d.r.percent >= 0 ? Fmt(L"%d%%", d.r.percent) : std::wstring(L"--");
        std::wstring text = DisplayName(d.r) + L"\t" + pct + L" · " + ConnName(d.r.conn);
        if (d.r.online && (d.r.charge == Charge::Charging || d.r.charge == Charge::Full))
            text += std::wstring(L" · ") + ChargeName(d.r.charge);
        if (d.r.online && d.r.percent < 0 && d.r.charge != Charge::Charging) text += L" · sin dato";
        else if (d.stale) text += L" · en reposo";
        mb.Item(menu, MF_STRING, ID_DEVICE_FIRST + idx, text, DeviceGlyph(d.r.kind));
        g->menuIds.push_back(d.r.id);
        ++idx;
    }
    if (idx == 0) mb.Item(menu, MF_STRING | MF_GRAYED, 0, L"No se detectaron dispositivos", kGlyphInfo);
    else mb.Item(menu, MF_STRING | MF_GRAYED, 0, L"Clic: renombrar · Arrastrar en la barra: mover", kGlyphInfo);
    mb.Separator(menu);
    mb.Item(menu, MF_STRING, ID_REFRESH, L"Actualizar ahora", kGlyphRefresh);

    mb.Item(menu, MF_STRING, ID_SETTINGS, L"Configuración…", kGlyphSettings);
    mb.Separator(menu);
    // Update status, bottom of the menu.
    if (g->updateInstalling) {
        mb.Item(menu, MF_STRING | MF_GRAYED, 0, L"Descargando actualización…", kGlyphDownload);
    } else if (g->update.ok && g->update.newer) {
        mb.Item(menu, MF_STRING, ID_UPDATE_INSTALL, L"Actualizar a la versión " + g->update.latest, kGlyphDownload);
    } else if (g->updateChecking) {
        mb.Item(menu, MF_STRING | MF_GRAYED, 0, L"Buscando actualizaciones…", kGlyphSync);
    } else if (g->update.ok) {
        mb.Item(menu, MF_STRING, ID_UPDATE_CHECK, L"App actualizada (BatteryHub " + CurrentVersion() + L")",
                kGlyphCheck);
    } else {
        mb.Item(menu, MF_STRING, ID_UPDATE_CHECK, L"Buscar actualizaciones (BatteryHub " + CurrentVersion() + L")",
                kGlyphSync);
    }
    mb.Item(menu, MF_STRING, ID_EXIT, L"Salir", kGlyphExit);

    SetForegroundWindow(g->hwnd);  // required so the menu closes when clicking elsewhere
    UINT flags = TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY;
    flags |= GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    UINT cmd = (UINT)TrackPopupMenuEx(menu, flags | TPM_BOTTOMALIGN, pt.x, pt.y, g->hwnd, nullptr);
    PostMessageW(g->hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);  // destroys submenus too; MenuBuilder frees the icons
    if (cmd) SendMessageW(g->hwnd, WM_COMMAND, cmd, 0);
}

void OnCommand(UINT id) {
    if (id >= ID_DEVICE_FIRST && id < ID_DEVICE_FIRST + g->menuIds.size()) {
        Rename(g->menuIds[id - ID_DEVICE_FIRST]);
    } else if (id == ID_UPDATE_CHECK) {
        StartUpdateCheck(true);
    } else if (id == ID_UPDATE_INSTALL) {
        StartUpdateInstall();
    } else if (id == ID_REFRESH) {
        StartPoll();
    } else if (id == ID_SETTINGS) {
        OpenSettings();
    } else if (id == ID_EXIT) {
        DestroyWindow(g->hwnd);
    }
}

void RegisterNotifications(HWND hwnd) {
    for (const GUID* guid : {&kHidInterface, &kBthDevice, &kBleDevice}) {
        DEV_BROADCAST_DEVICEINTERFACE_W f = {};
        f.dbcc_size = sizeof(f);
        f.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        f.dbcc_classguid = *guid;
        HDEVNOTIFY n = RegisterDeviceNotificationW(hwnd, &f, DEVICE_NOTIFY_WINDOW_HANDLE);
        if (n) g->notifications.push_back(n);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g && msg == g->taskbarCreated && msg != 0) {  // Explorer restarted
        g->widget.Destroy();  // its parent taskbar window is gone
        g->tray.ReAddAll();
        UpdateTray();
        return 0;
    }
    switch (msg) {
        case WM_APP_UPDATE_CHECKED: {
            std::unique_ptr<UpdateInfo> info((UpdateInfo*)lp);
            g->updateChecking = false;
            g->updateChecked = true;
            if (info->ok || !g->update.ok) g->update = *info;  // keep a good result over a failed retry
            RefreshSettingsWindow();
            if (wp) {  // the user asked: always answer
                if (info->ok && info->newer) {
                    if (MessageBoxW(hwnd,
                                    (L"Hay una versión nueva: " + info->latest + L" (tenés " + CurrentVersion() +
                                     L").\n\n¿Actualizar ahora? BatteryHub se reinicia solo.").c_str(),
                                    L"BatteryHub", MB_YESNO | MB_ICONINFORMATION | MB_TOPMOST) == IDYES)
                        StartUpdateInstall();
                } else if (info->ok) {
                    MessageBoxW(hwnd, (L"App actualizada: tenés la última versión (" + CurrentVersion() + L").").c_str(),
                                L"BatteryHub", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
                } else if (MessageBoxW(hwnd,
                                       (L"No se pudo buscar actualizaciones (" + info->error +
                                        L").\n\n¿Abrir la página de descargas?").c_str(),
                                       L"BatteryHub", MB_YESNO | MB_ICONWARNING | MB_TOPMOST) == IDYES) {
                    OpenReleasesPage();
                }
            }
            return 0;
        }

        case WM_APP_UPDATE_DONE: {
            std::unique_ptr<std::wstring> err((std::wstring*)lp);
            g->updateInstalling = false;
            RefreshSettingsWindow();
            if (wp && StartUpdatedApp()) {
                DestroyWindow(hwnd);  // the new version takes over once this instance is gone
                return 0;
            }
            if (MessageBoxW(hwnd,
                            (L"No se pudo actualizar automáticamente (" + *err +
                             L").\n\n¿Abrir la página de descargas para bajarla a mano?").c_str(),
                            L"BatteryHub", MB_YESNO | MB_ICONWARNING | MB_TOPMOST) == IDYES)
                OpenReleasesPage();
            return 0;
        }

        case WM_APP_REORDER: {
            // New order = what the widget shows now, then remembered ids not shown at the moment.
            std::vector<std::wstring> order;
            for (const auto& id : g->widget.Order())
                if (id != L"__none__") order.push_back(id);
            for (const auto& id : g->settings.order)
                if (std::find(order.begin(), order.end(), id) == order.end()) order.push_back(id);
            g->settings.order = order;
            g->settings.Save();
            UpdateTray();
            RefreshSettingsWindow();
            return 0;
        }

        case WM_APP_WIDGET_MENU: {
            POINT pt = {(int)(INT_PTR)wp, (int)(INT_PTR)lp};
            ShowMenu(pt);
            return 0;
        }

        case WM_APP_TRAY:
            switch (LOWORD(lp)) {
                case WM_CONTEXTMENU:
                case NIN_SELECT:
                case NIN_KEYSELECT: {
                    POINT pt = {GET_X_LPARAM(wp), GET_Y_LPARAM(wp)};
                    ShowMenu(pt);
                    return 0;
                }
            }
            return 0;

        case WM_APP_POLL_DONE: {
            std::unique_ptr<std::vector<BatteryReading>> res((std::vector<BatteryReading>*)lp);
            g->polling = false;
            g->devices.Apply(std::move(*res));
            UpdateTray();
            RefreshSettingsWindow();
            // Give back the pages touched by the poll (HID/SetupAPI buffers).
            SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
            if (g->pollAgain) {
                g->pollAgain = false;
                StartPoll();
            }
            return 0;
        }

        case WM_APP_WAKE:
            StartPoll();
            return 0;

        case WM_TIMER:
            if (wp == TIMER_UPDATE) {
                SetTimer(hwnd, TIMER_UPDATE, 24u * 60 * 60 * 1000, nullptr);  // then once a day
                StartUpdateCheck(false);
                return 0;
            }
            if (wp == TIMER_WIDGET) {
                // Game mode: needs 2 s of agreement before switching, so alt-tab doesn't flicker.
                bool full = g->settings.gameMode && IsFullscreenAppInFront();
                g->gameTicks = full != g->gaming ? g->gameTicks + 1 : 0;
                if (g->gameTicks >= 2) {
                    g->gameTicks = 0;
                    g->gaming = full;
                    if (full) {
                        KillTimer(hwnd, TIMER_POLL);
                        g->widget.Hide();
                        g->tray.RemoveAll();
                    } else {
                        RestartPollTimer();
                        UpdateTray();
                        StartPoll();
                    }
                    return 0;
                }
                if (g->gaming) return 0;
                if (g->widget.Ok() && !g->widget.Reposition()) {
                    g->widget.Destroy();
                    UpdateTray();  // re-embed, or fall back to tray icons
                } else if (!g->widget.Ok() && !g->settings.trayIcons) {
                    static int ticks = 0;
                    if (++ticks % 15 == 0) UpdateTray();  // retry embedding now and then
                }
                return 0;
            }
            if (wp == TIMER_DEBOUNCE) KillTimer(hwnd, TIMER_DEBOUNCE);
            StartPoll();
            return 0;

        case WM_DEVICECHANGE:
            if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE || wp == DBT_DEVNODES_CHANGED)
                Debounce(2000);  // devices enumerate in bursts; wait for them to settle
            return TRUE;

        case WM_POWERBROADCAST:
            if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) Debounce(6000);
            return TRUE;

        case WM_SETTINGCHANGE:
            if (lp && lstrcmpiW((LPCWSTR)lp, L"ImmersiveColorSet") == 0) UpdateTray();
            return 0;

        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            UpdateTray();
            return 0;

        case WM_COMMAND:
            OnCommand(LOWORD(wp));
            return 0;

        case WM_DESTROY:
            CloseSettingsWindow();
            KillTimer(hwnd, TIMER_POLL);
            KillTimer(hwnd, TIMER_DEBOUNCE);
            KillTimer(hwnd, TIMER_WIDGET);
            KillTimer(hwnd, TIMER_UPDATE);
            for (auto n : g->notifications) UnregisterDeviceNotification(n);
            g->widget.Destroy();
            g->tray.RemoveAll();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int RunTrayApp(HINSTANCE inst, bool afterUpdate) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\BatteryHub.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS && afterUpdate && mutex) {
        // Started by the updater: wait for the old version to exit and take over.
        DWORD w = WaitForSingleObject(mutex, 15000);
        if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) SetLastError(0);
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(kWindowClass, nullptr);
        if (other) PostMessageW(other, WM_APP_WAKE, 0, 0);  // just refresh the running one
        return 0;
    }

    AppState state;
    g = &state;
    g->inst = inst;
    g->settings.Load();
    g->devices.EnablePersistence();
    IconRendererInit();

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kWindowClass;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    RegisterClassExW(&wc);

    // A real (hidden) top-level window: message-only windows do not receive
    // the TaskbarCreated / WM_SETTINGCHANGE broadcasts.
    g->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"BatteryHub", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                              inst, nullptr);
    if (!g->hwnd) return 1;
    g->taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g->tray.Init(g->hwnd, WM_APP_TRAY);
    RegisterNotifications(g->hwnd);

    UpdateTray();  // placeholder icon right away, real ones after the first poll
    StartPoll();
    RestartPollTimer();
    SetTimer(g->hwnd, TIMER_WIDGET, 1000, nullptr);
    SetTimer(g->hwnd, TIMER_UPDATE, 30 * 1000, nullptr);  // first check shortly after start
    DeleteOldBinary();                                     // leftover of a previous update

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g->worker) {
        // A poll still running uses `g`; never free it under the worker's feet.
        // Icons are already removed, so a hard exit is safe if it does not finish.
        if (WaitForSingleObject(g->worker, 5000) == WAIT_TIMEOUT) ExitProcess(0);
        CloseHandle(g->worker);
    }
    // Drain a result the worker may have posted after the window died.
    while (PeekMessageW(&msg, nullptr, WM_APP_POLL_DONE, WM_APP_POLL_DONE, PM_REMOVE))
        delete (std::vector<BatteryReading>*)msg.lParam;

    IconRendererShutdown();
    g = nullptr;
    if (mutex) CloseHandle(mutex);
    return 0;
}

int RunDump() {
    OpenConsoleForOutput();
    g_verbose = true;
    Out(L"BatteryHub %ls --dump", BH_VERSION_WSTR);
    Out(L"Reporte de diagnóstico: pegalo completo al pedir soporte para un dispositivo nuevo.");

    DeviceManager dm;
    auto results = dm.PollAll();

    Out(L"");
    Out(L"==== Resumen: %zu dispositivo(s) con batería ====", results.size());
    for (const auto& r : results) {
        std::wstring pct = r.percent >= 0 ? Fmt(L"%3d%%", r.percent) : std::wstring(L" --");
        Out(L"  %ls  %-11ls %-36ls %-12ls %-12ls %ls  [%ls]  id=%ls", pct.c_str(), KindName(r.kind),
            r.name.c_str(), ConnName(r.conn), ChargeName(r.charge), r.online ? L"online " : L"offline",
            r.source.c_str(), r.id.c_str());
    }
    Out(L"");
    return 0;
}

}  // namespace bh
