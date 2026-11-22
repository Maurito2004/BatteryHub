// Tray application: hidden window, timers, device/power notifications, the
// background poll thread and the context menu.
#include "app.h"

#include <windows.h>
#include <windowsx.h>
#include <dbt.h>
#include <shellapi.h>

#include <memory>
#include <vector>

#include "../../assets/resource.h"
#include "../core/device_manager.h"
#include "../core/log.h"
#include "../core/settings.h"
#include "icon_renderer.h"
#include "taskbar_widget.h"
#include "tray.h"

namespace bh {
namespace {

constexpr wchar_t kWindowClass[] = L"BatteryHub.TrayWindow";
constexpr UINT WM_APP_TRAY = WM_APP + 1;
constexpr UINT WM_APP_POLL_DONE = WM_APP + 2;
constexpr UINT WM_APP_WAKE = WM_APP + 3;  // sent by a second instance
constexpr UINT WM_APP_WIDGET_MENU = WM_APP + 4;  // click on the taskbar widget (x, y)

constexpr UINT_PTR TIMER_POLL = 1;
constexpr UINT_PTR TIMER_DEBOUNCE = 2;
constexpr UINT_PTR TIMER_WIDGET = 3;  // keeps the widget anchored next to the clock

enum : UINT {
    ID_DEVICE_FIRST = 1000,  // + device index: rename
    ID_REFRESH = 2000,
    ID_HIDE_DISCONNECTED,
    ID_AUTOSTART,
    ID_EXIT,
    ID_MODE_TASKBAR,        // taskbar, left end
    ID_MODE_TASKBAR_CLOCK,  // taskbar, next to the clock
    ID_MODE_TRAY,
    ID_GAME_MODE,
    ID_GAME_MODE_BROWSERS,
    ID_INTERVAL_FIRST = 2100,   // + index into kIntervals
    ID_THRESHOLD_FIRST = 2200,  // + index into kThresholds
};
const int kIntervals[] = {30, 60, 120, 300, 600};
const int kThresholds[] = {5, 10, 15, 20, 25, 30};

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
        if (!g->widget.Ok()) g->widget.Create(g->inst, g->hwnd, WM_APP_WIDGET_MENU);
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
        if (DialogBoxParamW(g->inst, MAKEINTRESOURCEW(IDD_RENAME), g->hwnd, RenameProc, (LPARAM)&ctx) == IDOK)
            UpdateTray();
        return;
    }
}

// ---------------------------------------------------------------- context menu
void ShowMenu(POINT pt) {
    HMENU menu = CreatePopupMenu();
    g->menuIds.clear();

    UINT idx = 0;
    for (const auto& d : g->devices.Devices()) {
        if (!IsVisible(d)) continue;
        std::wstring pct = d.r.percent >= 0 ? Fmt(L"%d%%", d.r.percent) : std::wstring(L"--");
        std::wstring text = std::wstring(KindName(d.r.kind)) + L": " + DisplayName(d.r) + L"\t" + pct + L" · " +
                            ConnName(d.r.conn);
        if (d.r.online && (d.r.charge == Charge::Charging || d.r.charge == Charge::Full))
            text += std::wstring(L" · ") + ChargeName(d.r.charge);
        if (d.r.online && d.r.percent < 0 && d.r.charge != Charge::Charging) text += L" · sin dato de batería";
        else if (d.stale) text += L" · en reposo";
        AppendMenuW(menu, MF_STRING, ID_DEVICE_FIRST + idx, text.c_str());
        g->menuIds.push_back(d.r.id);
        ++idx;
    }
    if (idx == 0) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"No se detectaron dispositivos con batería");
    else AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"(clic en un dispositivo para renombrarlo)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_REFRESH, L"Actualizar ahora");

    HMENU interval = CreatePopupMenu();
    for (UINT i = 0; i < ARRAYSIZE(kIntervals); ++i) {
        int s = kIntervals[i];
        std::wstring label = s < 60 ? Fmt(L"%d segundos", s) : Fmt(L"%d minuto%ls", s / 60, s >= 120 ? L"s" : L"");
        AppendMenuW(interval, MF_STRING | (g->settings.intervalSec == s ? MF_CHECKED : 0), ID_INTERVAL_FIRST + i,
                    label.c_str());
    }
    HMENU threshold = CreatePopupMenu();
    for (UINT i = 0; i < ARRAYSIZE(kThresholds); ++i) {
        int t = kThresholds[i];
        AppendMenuW(threshold, MF_STRING | (g->settings.lowThreshold == t ? MF_CHECKED : 0), ID_THRESHOLD_FIRST + i,
                    Fmt(L"%d %%", t).c_str());
    }
    HMENU mode = CreatePopupMenu();
    AppendMenuW(mode, MF_STRING, ID_MODE_TASKBAR, L"Barra de tareas, a la izquierda");
    AppendMenuW(mode, MF_STRING, ID_MODE_TASKBAR_CLOCK, L"Barra de tareas, junto al reloj");
    AppendMenuW(mode, MF_STRING, ID_MODE_TRAY, L"Área de notificación (un ícono por dispositivo)");
    UINT current = g->settings.trayIcons ? ID_MODE_TRAY
                 : g->settings.widgetLeft ? ID_MODE_TASKBAR : ID_MODE_TASKBAR_CLOCK;
    CheckMenuRadioItem(mode, ID_MODE_TASKBAR, ID_MODE_TRAY, current, MF_BYCOMMAND);
    HMENU config = CreatePopupMenu();
    AppendMenuW(config, MF_POPUP, (UINT_PTR)mode, L"Mostrar en");
    AppendMenuW(config, MF_POPUP, (UINT_PTR)interval, L"Intervalo de actualización");
    AppendMenuW(config, MF_POPUP, (UINT_PTR)threshold, L"Umbral de batería baja");
    AppendMenuW(config, MF_STRING | (g->settings.hideDisconnected ? MF_CHECKED : 0), ID_HIDE_DISCONNECTED,
                L"Ocultar dispositivos en reposo");
    AppendMenuW(config, MF_STRING | (g->settings.gameMode ? MF_CHECKED : 0), ID_GAME_MODE,
                L"Ocultar durante juegos a pantalla completa");
    AppendMenuW(config,
                MF_STRING | (g->settings.gameModeBrowsers ? MF_CHECKED : 0) | (g->settings.gameMode ? 0 : MF_GRAYED),
                ID_GAME_MODE_BROWSERS, L"Ocultar también con el navegador a pantalla completa");
    AppendMenuW(config, MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : 0), ID_AUTOSTART, L"Iniciar con Windows");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)config, L"Configuración");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"Salir");

    // Radio-style marks for the two single-choice submenus.
    for (UINT i = 0; i < ARRAYSIZE(kIntervals); ++i)
        if (g->settings.intervalSec == kIntervals[i])
            CheckMenuRadioItem(interval, ID_INTERVAL_FIRST, ID_INTERVAL_FIRST + ARRAYSIZE(kIntervals) - 1,
                               ID_INTERVAL_FIRST + i, MF_BYCOMMAND);
    for (UINT i = 0; i < ARRAYSIZE(kThresholds); ++i)
        if (g->settings.lowThreshold == kThresholds[i])
            CheckMenuRadioItem(threshold, ID_THRESHOLD_FIRST, ID_THRESHOLD_FIRST + ARRAYSIZE(kThresholds) - 1,
                               ID_THRESHOLD_FIRST + i, MF_BYCOMMAND);

    SetForegroundWindow(g->hwnd);  // required so the menu closes when clicking elsewhere
    UINT flags = TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY;
    flags |= GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    UINT cmd = (UINT)TrackPopupMenuEx(menu, flags | TPM_BOTTOMALIGN, pt.x, pt.y, g->hwnd, nullptr);
    PostMessageW(g->hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);  // destroys submenus too
    if (cmd) SendMessageW(g->hwnd, WM_COMMAND, cmd, 0);
}

void OnCommand(UINT id) {
    if (id >= ID_DEVICE_FIRST && id < ID_DEVICE_FIRST + g->menuIds.size()) {
        Rename(g->menuIds[id - ID_DEVICE_FIRST]);
    } else if (id == ID_REFRESH) {
        StartPoll();
    } else if (id >= ID_INTERVAL_FIRST && id < ID_INTERVAL_FIRST + ARRAYSIZE(kIntervals)) {
        g->settings.intervalSec = kIntervals[id - ID_INTERVAL_FIRST];
        g->settings.Save();
        RestartPollTimer();
    } else if (id >= ID_THRESHOLD_FIRST && id < ID_THRESHOLD_FIRST + ARRAYSIZE(kThresholds)) {
        g->settings.lowThreshold = kThresholds[id - ID_THRESHOLD_FIRST];
        g->settings.Save();
        UpdateTray();
    } else if (id == ID_MODE_TASKBAR || id == ID_MODE_TASKBAR_CLOCK || id == ID_MODE_TRAY) {
        g->settings.trayIcons = id == ID_MODE_TRAY;
        if (id != ID_MODE_TRAY) g->settings.widgetLeft = id == ID_MODE_TASKBAR;
        g->settings.Save();
        UpdateTray();
    } else if (id == ID_GAME_MODE) {
        g->settings.gameMode = !g->settings.gameMode;
        g->settings.Save();
    } else if (id == ID_GAME_MODE_BROWSERS) {
        g->settings.gameModeBrowsers = !g->settings.gameModeBrowsers;
        g->settings.Save();
    } else if (id == ID_HIDE_DISCONNECTED) {
        g->settings.hideDisconnected = !g->settings.hideDisconnected;
        g->settings.Save();
        UpdateTray();
    } else if (id == ID_AUTOSTART) {
        if (!SetAutostart(!IsAutostartEnabled()))
            MessageBoxW(g->hwnd, L"No se pudo modificar el inicio automático.", L"BatteryHub", MB_ICONWARNING);
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
            KillTimer(hwnd, TIMER_POLL);
            KillTimer(hwnd, TIMER_DEBOUNCE);
            KillTimer(hwnd, TIMER_WIDGET);
            for (auto n : g->notifications) UnregisterDeviceNotification(n);
            g->widget.Destroy();
            g->tray.RemoveAll();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int RunTrayApp(HINSTANCE inst) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\BatteryHub.SingleInstance");
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
