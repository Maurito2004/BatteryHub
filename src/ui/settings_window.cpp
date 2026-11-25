// Settings window, drawn with Direct2D + DirectWrite.
//
// Layout: a sidebar (brand, sections, a live summary of the devices) and a
// scrolling content area made of cards. Each section is rebuilt from the
// current settings on every paint, so the window never holds stale copies.
// Small animations (battery bars filling when a section opens, toggle knobs,
// the selected pill of a segmented control sliding) run on a 15 ms timer that
// only exists while something is still moving.
#ifndef NOMINMAX
#define NOMINMAX  // std::min / std::max, not the windows.h macros
#endif
#include "settings_window.h"

#include <d2d1.h>
#include <dwrite.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <map>

#include "../../assets/resource.h"
#include "../core/settings.h"
#include "../core/updater.h"
#include "icon_renderer.h"

#ifdef _MSC_VER
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#endif

namespace bh {
namespace {

constexpr wchar_t kClass[] = L"BatteryHub.Settings";
constexpr UINT_PTR TIMER_ANIM = 1;
constexpr float kSidebarW = 232.0f;
constexpr float kPad = 30.0f;
constexpr float kContentMaxW = 680.0f;

const int kIntervals[] = {30, 60, 120, 300, 600};
constexpr int kThresholdMin = 5, kThresholdMax = 30, kThresholdStep = 5;

// ------------------------------------------------------------------ geometry
struct R {
    float l = 0, t = 0, r = 0, b = 0;
    R() = default;
    R(float l_, float t_, float r_, float b_) : l(l_), t(t_), r(r_), b(b_) {}
    float w() const { return r - l; }
    float h() const { return b - t; }
    bool Has(float x, float y) const { return x >= l && x < r && y >= t && y < b; }
    D2D1_RECT_F D() const { return D2D1::RectF(l, t, r, b); }
};

struct C {
    float r, g, b;
};
C Hex(unsigned v) { return {((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f}; }
C Mix(C a, C b, float k) { return {a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k}; }

// Graphite with BatteryHub's charge green as the accent (the same green as the
// charging bolt in the taskbar), amber and red for the battery levels.
struct Pal {
    C bg, side, card, line, sel, accent, onAccent, text, dim, ctrl, ctrlHover, warn, low;
};
Pal MakePal(bool dark) {
    if (dark)
        return {Hex(0x111315), Hex(0x16191B), Hex(0x1C1F22), Hex(0x2B3034), Hex(0x223026), Hex(0x6CCB5F),
                Hex(0x0C1A0A), Hex(0xF1F3F2), Hex(0x9BA3A0), Hex(0x2A2F33), Hex(0x363C41), Hex(0xF2B33D),
                Hex(0xFF5C50)};
    return {Hex(0xF3F4F2), Hex(0xE8EBE7), Hex(0xFFFFFF), Hex(0xDDE1DC), Hex(0xD9ECD5), Hex(0x107C10),
            Hex(0xFFFFFF), Hex(0x1A1C1B), Hex(0x5F6763), Hex(0xE6E9E5), Hex(0xD8DDD6), Hex(0xB86E00),
            Hex(0xC42B1C)};
}

// ------------------------------------------------------------------ model
enum ActType { A_NONE, A_SECTION, A_TOGGLE, A_CHOOSE, A_SLIDER, A_CMD, A_RENAME, A_UP, A_DOWN };
struct Act {
    int t = A_NONE, a = 0, b = 0;
    bool operator==(const Act& o) const { return t == o.t && a == o.a && b == o.b; }
    bool None() const { return t == A_NONE; }
};

enum Key { K_MODE = 1, K_THEME, K_HIDE, K_INTERVAL, K_THRESHOLD, K_GAME, K_GAME_BROWSERS, K_AUTOSTART };
enum Cmd { C_REFRESH = 1, C_CHECK, C_INSTALL, C_RELEASES, C_REPO, C_ISSUES };

enum class Ctl { None, Toggle, Choice, Slider, Buttons, Device, Text };
struct Row {
    std::wstring title, desc;
    Ctl ctl = Ctl::None;
    int key = 0;
    bool on = false, disabled = false;
    std::vector<std::wstring> opts;
    int sel = -1;
    int value = 0;                                     // slider
    std::vector<std::pair<std::wstring, int>> buttons;  // first one is the primary action
    int dev = -1;                                      // index into the device list
    std::wstring right;                                // Text: value on the right
    bool glyph = false;                                // Text: `right` is an icon-font glyph
};
struct Group {
    std::wstring title;
    std::vector<Row> rows;
};

struct SectionDef {
    const wchar_t* glyph;
    const wchar_t* name;
    const wchar_t* sub;
};
const SectionDef kSections[] = {
    {L"", L"Dispositivos", L"Lo que BatteryHub encontró, en el mismo orden que en la barra de tareas."},
    {L"", L"Barra de tareas", L"Dónde aparecen los niveles y cómo se ven."},
    {L"", L"Lectura y alertas", L"Cada cuánto se consulta y cuándo una batería cuenta como baja."},
    {L"", L"Modo juego", L"Para que nada aparezca encima de tus partidas."},
    {L"", L"General", L"Inicio con Windows y actualizaciones."},
    {L"", L"Acerca de", L"Versión, código fuente y cómo reportar un dispositivo."},
};
constexpr int kSectionCount = (int)(sizeof(kSections) / sizeof(kSections[0]));

// ------------------------------------------------------------------ state
struct Win {
    HWND hwnd = nullptr;
    HINSTANCE inst = nullptr;
    SettingsHost* host = nullptr;
    int section = 0;
    Act hover, pressed;
    int dragKey = 0;
    bool tracking = false, timer = false, refreshing = false, dark = true;
    float scroll = 0, contentH = 0, scale = 1;
    ULONGLONG animStart = 0;
    std::vector<std::pair<R, Act>> hits;
    std::map<int, R> sliderTrack;
    std::map<int, float> knob;                  // toggle key -> 0..1
    std::map<int, std::pair<float, float>> seg;  // choice key -> animated pill (left, right)
    std::vector<DeviceView> devices;

    ID2D1Factory* factory = nullptr;
    ID2D1HwndRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    IDWriteFactory* dw = nullptr;
    IDWriteTextFormat *fBrand = nullptr, *fTitle = nullptr, *fSub = nullptr, *fGroup = nullptr, *fRow = nullptr,
                      *fRowB = nullptr, *fDesc = nullptr, *fSmall = nullptr, *fCtrl = nullptr, *fCtrlR = nullptr,
                      *fPct = nullptr, *fIcon = nullptr, *fIconC = nullptr, *fIconL = nullptr;
};
Win* w = nullptr;
Pal P;
float g_alpha = 1.0f;  // multiplies every color: dims disabled rows and sleeping devices

template <class T>
void SafeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

bool IsDark(int theme) {
    if (theme == 1) return true;
    if (theme == 2) return false;
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

// Dark or light title bar (DWMWA_USE_IMMERSIVE_DARK_MODE, Windows 10 20H1+ / 11).
void SetTitleBar(HWND hwnd, bool dark) {
    static HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dwm) return;
    using Fn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    static auto set = (Fn)(void*)GetProcAddress(dwm, "DwmSetWindowAttribute");
    BOOL v = dark;
    if (set) set(hwnd, 20, &v, sizeof(v));
}

void ApplyTheme() {
    w->dark = IsDark(w->host->Prefs().menuTheme);
    P = MakePal(w->dark);
    SetTitleBar(w->hwnd, w->dark);
}

// ------------------------------------------------------------------ graphics
bool FontExists(const wchar_t* family) {
    IDWriteFontCollection* fc = nullptr;
    if (FAILED(w->dw->GetSystemFontCollection(&fc, FALSE)) || !fc) return false;
    UINT32 idx = 0;
    BOOL exists = FALSE;
    fc->FindFamilyName(family, &idx, &exists);
    fc->Release();
    return exists != FALSE;
}

IDWriteTextFormat* MakeFormat(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight,
                              DWRITE_TEXT_ALIGNMENT align, bool wrap = false) {
    IDWriteTextFormat* f = nullptr;
    if (FAILED(w->dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                       size, L"es-ar", &f)))
        return nullptr;
    f->SetTextAlignment(align);
    f->SetParagraphAlignment(wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!wrap) {  // long names end in "…" instead of being cut
        DWRITE_TRIMMING trim = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        IDWriteInlineObject* sign = nullptr;
        if (SUCCEEDED(w->dw->CreateEllipsisTrimmingSign(f, &sign)) && sign) {
            f->SetTrimming(&trim, sign);
            sign->Release();
        }
    }
    return f;
}

bool EnsureFonts() {
    if (w->fBrand) return true;
    if (!w->dw && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                             reinterpret_cast<IUnknown**>(&w->dw))))
        return false;
    const wchar_t* ui = FontExists(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    const wchar_t* disp = FontExists(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
    const wchar_t* icons = FontExists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    const auto Lead = DWRITE_TEXT_ALIGNMENT_LEADING, Cn = DWRITE_TEXT_ALIGNMENT_CENTER,
               Rt = DWRITE_TEXT_ALIGNMENT_TRAILING;
    w->fBrand = MakeFormat(disp, 17.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, Lead);
    w->fTitle = MakeFormat(disp, 27.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, Lead);
    w->fSub = MakeFormat(ui, 13.5f, DWRITE_FONT_WEIGHT_NORMAL, Lead);
    w->fGroup = MakeFormat(ui, 11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, Lead);
    w->fRow = MakeFormat(ui, 14.0f, DWRITE_FONT_WEIGHT_NORMAL, Lead);
    w->fRowB = MakeFormat(ui, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, Lead);
    w->fDesc = MakeFormat(ui, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, Lead, true);
    w->fSmall = MakeFormat(ui, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, Lead);
    w->fCtrl = MakeFormat(ui, 13.0f, DWRITE_FONT_WEIGHT_NORMAL, Cn);
    w->fCtrlR = MakeFormat(ui, 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, Rt);
    w->fPct = MakeFormat(disp, 17.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, Rt);
    w->fIcon = MakeFormat(icons, 16.0f, DWRITE_FONT_WEIGHT_NORMAL, Cn);
    w->fIconC = MakeFormat(icons, 13.0f, DWRITE_FONT_WEIGHT_NORMAL, Cn);
    w->fIconL = MakeFormat(icons, 19.0f, DWRITE_FONT_WEIGHT_NORMAL, Cn);
    return w->fBrand && w->fTitle && w->fSub && w->fGroup && w->fRow && w->fRowB && w->fDesc && w->fSmall &&
           w->fCtrl && w->fCtrlR && w->fPct && w->fIcon && w->fIconC && w->fIconL;
}

void ReleaseTarget() {
    SafeRelease(w->brush);
    SafeRelease(w->rt);
}

void ReleaseAll() {
    ReleaseTarget();
    for (IDWriteTextFormat** f : {&w->fBrand, &w->fTitle, &w->fSub, &w->fGroup, &w->fRow, &w->fRowB, &w->fDesc,
                                  &w->fSmall, &w->fCtrl, &w->fCtrlR, &w->fPct, &w->fIcon, &w->fIconC, &w->fIconL})
        SafeRelease(*f);
    SafeRelease(w->dw);
    SafeRelease(w->factory);
}

bool EnsureGfx() {
    if (w->rt) return true;
    // Explicit-IID form: the template overload is not in every SDK (MinGW builds in CI).
    if (!w->factory && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr,
                                                reinterpret_cast<void**>(&w->factory))))
        return false;
    if (!EnsureFonts()) return false;
    RECT rc;
    GetClientRect(w->hwnd, &rc);
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), 96.0f * w->scale,
                                              96.0f * w->scale);
    auto hprops = D2D1::HwndRenderTargetProperties(
        w->hwnd, D2D1::SizeU((UINT32)std::max<LONG>(1, rc.right), (UINT32)std::max<LONG>(1, rc.bottom)));
    if (FAILED(w->factory->CreateHwndRenderTarget(props, hprops, &w->rt))) return false;
    if (FAILED(w->rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &w->brush))) {
        ReleaseTarget();
        return false;
    }
    return true;
}

float TextW(const std::wstring& s, IDWriteTextFormat* f) {
    IDWriteTextLayout* l = nullptr;
    if (s.empty() || FAILED(w->dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 4000, 100, &l)) || !l)
        return s.size() * 7.0f;
    DWRITE_TEXT_METRICS m = {};
    l->GetMetrics(&m);
    l->Release();
    return m.widthIncludingTrailingWhitespace;
}

float TextH(const std::wstring& s, IDWriteTextFormat* f, float width) {
    IDWriteTextLayout* l = nullptr;
    if (s.empty() || FAILED(w->dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, std::max(width, 40.0f), 2000, &l)) ||
        !l)
        return 18.0f;
    DWRITE_TEXT_METRICS m = {};
    l->GetMetrics(&m);
    l->Release();
    return m.height;
}

void Color(C c, float a) { w->brush->SetColor(D2D1::ColorF(c.r, c.g, c.b, a * g_alpha)); }
void Fill(R r, C c, float a = 1) {
    Color(c, a);
    w->rt->FillRectangle(r.D(), w->brush);
}
void Round(R r, float rad, C c, float a = 1) {
    Color(c, a);
    w->rt->FillRoundedRectangle(D2D1::RoundedRect(r.D(), rad, rad), w->brush);
}
void Outline(R r, float rad, C c, float a, float width) {
    Color(c, a);
    w->rt->DrawRoundedRectangle(D2D1::RoundedRect(r.D(), rad, rad), w->brush, width);
}
void Circle(float cx, float cy, float rad, C c, float a = 1) {
    Color(c, a);
    w->rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), w->brush);
}
void Ring(float cx, float cy, float rad, C c, float a, float width) {
    Color(c, a);
    w->rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), w->brush, width);
}
void Text(const std::wstring& s, IDWriteTextFormat* f, R r, C c, float a = 1) {
    if (s.empty() || !f) return;
    Color(c, a);
    w->rt->DrawText(s.c_str(), (UINT32)s.size(), f, r.D(), w->brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float EaseOut(float t) { return 1 - (1 - t) * (1 - t) * (1 - t); }

C LevelColor(int pct) {
    int low = w->host->Prefs().lowThreshold;
    if (pct < 0) return P.dim;
    if (pct <= low) return P.low;
    if (pct <= low + 15) return P.warn;
    return P.accent;
}

std::wstring Pct(int p) { return p >= 0 ? std::to_wstring(p) + L"%" : L"--"; }

// ------------------------------------------------------------------ sections
std::vector<Group> BuildSection(int s) {
    Settings& st = w->host->Prefs();
    std::vector<Group> out;
    auto toggle = [](const wchar_t* t, const wchar_t* d, int key, bool on, bool disabled = false) {
        Row r;
        r.title = t, r.desc = d, r.ctl = Ctl::Toggle, r.key = key, r.on = on, r.disabled = disabled;
        return r;
    };
    auto choice = [](const wchar_t* t, const wchar_t* d, int key, std::vector<std::wstring> opts, int sel) {
        Row r;
        r.title = t, r.desc = d, r.ctl = Ctl::Choice, r.key = key, r.opts = std::move(opts), r.sel = sel;
        return r;
    };
    auto buttons = [](std::wstring t, std::wstring d, std::vector<std::pair<std::wstring, int>> b) {
        Row r;
        r.title = std::move(t), r.desc = std::move(d), r.ctl = Ctl::Buttons, r.buttons = std::move(b);
        return r;
    };
    switch (s) {
        case 0: {
            Group g;
            for (size_t i = 0; i < w->devices.size(); ++i) {
                Row r;
                r.ctl = Ctl::Device;
                r.dev = (int)i;
                g.rows.push_back(r);
            }
            if (g.rows.empty()) {
                Row r;
                r.title = L"Todavía no hay dispositivos con batería";
                r.desc = L"Encendé o conectá un mouse, auricular, teclado o mando: aparece solo en unos segundos. "
                         L"Si no aparece, mirá «Acerca de» para reportarlo.";
                g.rows.push_back(r);
            }
            out.push_back(std::move(g));
            Group tips{L"Atajos"};
            Row t1;
            t1.title = L"Renombrar";
            t1.desc = L"El lápiz le pone el nombre que quieras; vacío vuelve al original.";
            t1.ctl = Ctl::Text;
            t1.right = L"\uE70F";
            t1.glyph = true;
            Row t2;
            t2.title = L"Ordenar";
            t2.desc = L"Las flechas lo mueven. También podés arrastrarlos directamente en la barra de tareas.";
            t2.ctl = Ctl::Text;
            t2.right = L"\uE8CB";
            t2.glyph = true;
            tips.rows.push_back(t1);
            tips.rows.push_back(t2);
            out.push_back(std::move(tips));
            break;
        }
        case 1: {
            Group where{L"Ubicación"};
            int mode = st.trayIcons ? 2 : st.widgetLeft ? 0 : 1;
            where.rows.push_back(choice(L"Mostrar en",
                                        L"En la barra de tareas queda como texto al lado de tus íconos; en la bandeja, "
                                        L"un ícono por dispositivo junto al reloj.",
                                        K_MODE, {L"Izquierda", L"Junto al reloj", L"Bandeja"}, mode));
            out.push_back(std::move(where));
            Group look{L"Apariencia"};
            look.rows.push_back(choice(L"Tema", L"Para el menú del clic y para esta ventana.", K_THEME,
                                       {L"Como Windows", L"Oscuro", L"Claro"}, st.menuTheme));
            look.rows.push_back(toggle(L"Ocultar dispositivos en reposo",
                                       L"Los que no responden dejan de mostrarse hasta que vuelvan a despertarse.",
                                       K_HIDE, st.hideDisconnected));
            out.push_back(std::move(look));
            break;
        }
        case 2: {
            Group read{L"Lectura"};
            int sel = -1;
            for (int i = 0; i < (int)(sizeof(kIntervals) / sizeof(kIntervals[0])); ++i)
                if (kIntervals[i] == st.intervalSec) sel = i;
            read.rows.push_back(choice(L"Consultar cada",
                                       L"También se lee al instante cuando conectás o desconectás algo.", K_INTERVAL,
                                       {L"30 s", L"1 min", L"2 min", L"5 min", L"10 min"}, sel));
            out.push_back(std::move(read));
            Group low{L"Batería baja"};
            Row r;
            r.title = L"Avisar desde";
            r.desc = L"Igual o por debajo de este nivel el porcentaje se pone rojo. La zona roja de la barra es lo "
                     L"que cuenta como bajo.";
            r.ctl = Ctl::Slider;
            r.key = K_THRESHOLD;
            r.value = st.lowThreshold;
            low.rows.push_back(r);
            out.push_back(std::move(low));
            break;
        }
        case 3: {
            Group g;
            g.rows.push_back(toggle(L"Ocultar durante juegos a pantalla completa",
                                    L"Se esconde y deja de consultar los dispositivos hasta que salgas del juego. "
                                    L"Una ventana maximizada normal no cuenta como juego.",
                                    K_GAME, st.gameMode));
            g.rows.push_back(toggle(L"También con el navegador a pantalla completa",
                                    L"Por defecto un video en pantalla completa no lo esconde.", K_GAME_BROWSERS,
                                    st.gameModeBrowsers, !st.gameMode));
            out.push_back(std::move(g));
            break;
        }
        case 4: {
            Group start{L"Inicio"};
            start.rows.push_back(toggle(L"Iniciar con Windows",
                                        L"Arranca solo al iniciar sesión. No necesita permisos de administrador.",
                                        K_AUTOSTART, w->host->Autostart()));
            out.push_back(std::move(start));
            Group upd{L"Actualizaciones"};
            UpdateView u = w->host->Update();
            std::wstring title = L"BatteryHub " + CurrentVersion();
            switch (u.state) {
                case UpdateView::Available:
                    upd.rows.push_back(buttons(title, L"Hay una versión nueva: " + u.latest +
                                                          L". Se descarga, reemplaza el exe y se reinicia sola.",
                                               {{L"Actualizar a " + u.latest, C_INSTALL}, {L"Ver novedades", C_RELEASES}}));
                    break;
                case UpdateView::Checking: {
                    Row r;
                    r.title = title, r.desc = L"Buscando actualizaciones…";
                    upd.rows.push_back(r);
                    break;
                }
                case UpdateView::Installing: {
                    Row r;
                    r.title = title, r.desc = L"Descargando la actualización… BatteryHub se reinicia cuando termine.";
                    upd.rows.push_back(r);
                    break;
                }
                case UpdateView::UpToDate:
                    upd.rows.push_back(buttons(title, L"Tenés la última versión. Se vuelve a revisar una vez por día.",
                                               {{L"Buscar de nuevo", C_CHECK}}));
                    break;
                case UpdateView::Failed:
                    upd.rows.push_back(buttons(title, L"No se pudo consultar GitHub. Revisá la conexión o bajala a mano.",
                                               {{L"Reintentar", C_CHECK}, {L"Abrir descargas", C_RELEASES}}));
                    break;
                default:
                    upd.rows.push_back(buttons(title, L"Se revisa sola al arrancar y una vez por día.",
                                               {{L"Buscar actualizaciones", C_CHECK}}));
            }
            out.push_back(std::move(upd));
            break;
        }
        case 5: {
            Group g;
            Row v;
            v.title = L"Versión";
            v.ctl = Ctl::Text;
            v.right = CurrentVersion();
            g.rows.push_back(v);
            Row m;
            m.title = L"Consumo";
            m.ctl = Ctl::Text;
            m.right = L"~3 MB · sin servicios";
            g.rows.push_back(m);
            g.rows.push_back(buttons(L"Código fuente", L"BatteryHub es gratis y de código abierto (licencia MIT).",
                                     {{L"Abrir en GitHub", C_REPO}}));
            g.rows.push_back(buttons(L"¿Un dispositivo no aparece?",
                                     L"Corré «BatteryHub.exe --dump > dump.txt» en una terminal y adjuntá el archivo en "
                                     L"un issue: con eso se puede agregar soporte.",
                                     {{L"Reportar un dispositivo", C_ISSUES}}));
            out.push_back(std::move(g));
            break;
        }
    }
    return out;
}

// ------------------------------------------------------------------ rows
float SegW(const std::wstring& s) { return TextW(s, w->fCtrl) + 28; }
float BtnW(const std::wstring& s) { return TextW(s, w->fCtrl) + 36; }

float CtlWidth(const Row& row) {
    switch (row.ctl) {
        case Ctl::Toggle: return 46;
        case Ctl::Choice: {
            float s = 8;
            for (const auto& o : row.opts) s += SegW(o);
            return s;
        }
        case Ctl::Slider: return 270;
        case Ctl::Text: return row.glyph ? 28.0f : TextW(row.right, w->fCtrlR) + 4;
        default: return 0;
    }
}

float RowHeight(const Row& row, float width) {
    if (row.ctl == Ctl::Device) return 72;
    float cw = CtlWidth(row);
    float tw = width - 36 - (cw > 0 ? cw + 24 : 0);
    float dh = row.desc.empty() ? 0 : TextH(row.desc, w->fDesc, tw);
    float h = 15 + 20 + (dh > 0 ? 4 + dh : 0) + 15;
    if (row.ctl == Ctl::Buttons && !row.buttons.empty()) h += 46;
    return std::max(h, row.desc.empty() ? 54.0f : 64.0f);
}

void Pill(const std::wstring& label, R r, Act a, bool primary) {
    bool hv = w->hover == a;
    if (primary) Round(r, r.h() / 2, P.accent, hv ? 0.86f : 1.0f);
    else Round(r, r.h() / 2, hv ? P.ctrlHover : P.ctrl);
    Text(label, w->fCtrl, r, primary ? P.onAccent : P.text);
    w->hits.push_back({r, a});
}

void IconButton(const wchar_t* glyph, R b, Act a, bool enabled) {
    if (!enabled) {
        Text(glyph, w->fIconC, b, P.dim, 0.28f);
        return;
    }
    bool hv = w->hover == a;
    if (hv) Round(b, b.h() / 2, P.ctrlHover);
    Text(glyph, w->fIconC, b, hv ? P.text : P.dim);
    w->hits.push_back({b, a});
}

void DrawDevice(const Row& row, R r, float fill, bool& animating) {
    (void)animating;
    const DeviceView& d = w->devices[row.dev];
    const int n = (int)w->devices.size(), i = row.dev;
    const float saved = g_alpha;
    if (d.stale) g_alpha *= 0.62f;
    const float cy = (r.t + r.b) / 2;
    const C lvl = LevelColor(d.percent);

    // Device glyph in a tinted circle.
    float cx = r.l + 18 + 21;
    Circle(cx, cy, 21, lvl, 0.15f);
    Text(DeviceGlyph(d.kind), w->fIconL, R(cx - 21, cy - 21, cx + 21, cy + 21), lvl);

    // Right to left: rename, down, up, percentage, bar.
    float bx = r.r - 14;
    auto next = [&](float width) {
        R b(bx - width, cy - 15, bx, cy + 15);
        bx -= width + 2;
        return b;
    };
    IconButton(L"", next(30), {A_RENAME, i}, true);
    if (n > 1) {
        IconButton(L"", next(30), {A_DOWN, i}, i < n - 1);
        IconButton(L"", next(30), {A_UP, i}, i > 0);
    }
    bx -= 12;
    std::wstring pct = Pct(d.percent);
    R pr(bx - 54, cy - 14, bx, cy + 14);
    Text(pct, w->fPct, pr, d.percent >= 0 && d.percent <= w->host->Prefs().lowThreshold ? P.low : P.text);
    float left = pr.l;
    if (d.charging) {
        float pw = TextW(pct, w->fPct);
        R bolt(pr.r - pw - 22, cy - 12, pr.r - pw - 4, cy + 12);
        Text(L"", w->fIconC, bolt, P.accent);
        left = std::min(left, bolt.l);
    }
    float nameRight = left - 14;
    if (r.w() > 540) {  // the bar needs room; narrow windows keep just the number
        R bar(left - 16 - 120, cy - 4, left - 16, cy + 4);
        Round(bar, 4, P.ctrl);
        if (d.percent >= 0) {
            float fw = std::max(bar.h(), bar.w() * d.percent / 100.0f * fill);
            Round(R(bar.l, bar.t, bar.l + fw, bar.b), 4, lvl);
        }
        nameRight = bar.l - 16;
    }

    float tx = r.l + 74;
    Text(d.name, w->fRowB, R(tx, r.t + 15, nameRight, r.t + 37), P.text);
    Text(d.detail, w->fSmall, R(tx, r.t + 37, nameRight, r.t + 56), P.dim);
    g_alpha = saved;
}

void DrawRow(const Row& row, R r, float fill, bool& animating) {
    if (row.ctl == Ctl::Device) {
        DrawDevice(row, r, fill, animating);
        return;
    }
    const float saved = g_alpha;
    if (row.disabled) g_alpha *= 0.42f;
    const float tx = r.l + 18;
    const float cw = CtlWidth(row);
    const float tw = r.w() - 36 - (cw > 0 ? cw + 24 : 0);
    const bool buttons = row.ctl == Ctl::Buttons && !row.buttons.empty();
    const bool compact = row.desc.empty() && !buttons;
    if (compact) {
        Text(row.title, w->fRow, R(tx, r.t, tx + tw, r.b), P.text);
    } else {
        Text(row.title, w->fRow, R(tx, r.t + 15, tx + tw, r.t + 36), P.text);
        float dh = TextH(row.desc, w->fDesc, tw);
        Text(row.desc, w->fDesc, R(tx, r.t + 39, tx + tw, r.t + 39 + dh + 2), P.dim);
    }
    const float cy = (r.t + r.b) / 2;
    const float right = r.r - 18;
    switch (row.ctl) {
        case Ctl::Toggle: {
            R t(right - 46, cy - 12, right, cy + 12);
            Act a{A_TOGGLE, row.key};
            bool hv = !row.disabled && w->hover == a;
            float target = row.on ? 1.0f : 0.0f;
            auto it = w->knob.find(row.key);
            float k = it == w->knob.end() ? target : it->second;
            k += (target - k) * 0.32f;
            if (std::fabs(target - k) < 0.01f) k = target;
            else animating = true;
            w->knob[row.key] = k;
            Round(t, 12, Mix(hv ? P.ctrlHover : P.ctrl, P.accent, k));
            if (k < 1) Outline(t, 12, P.dim, 0.55f * (1 - k), 1);
            float kx = t.l + 12 + (t.w() - 24) * k;
            Circle(kx, cy, hv ? 7.5f : 7.0f, Mix(P.dim, P.onAccent, k));
            if (!row.disabled) w->hits.push_back({R(r.l, r.t, r.r, r.b), a});  // the whole row flips it
            break;
        }
        case Ctl::Choice: {
            R c(right - cw, cy - 17, right, cy + 17);
            Round(c, 10, P.ctrl);
            std::vector<R> segs;
            float x = c.l + 4;
            for (const auto& o : row.opts) {
                float sw = SegW(o);
                segs.emplace_back(x, c.t + 4, x + sw, c.b - 4);
                x += sw;
            }
            for (size_t i = 0; i < segs.size(); ++i) {
                Act a{A_CHOOSE, row.key, (int)i};
                if ((int)i != row.sel && w->hover == a) Round(segs[i], 7, P.ctrlHover);
            }
            if (row.sel >= 0 && row.sel < (int)segs.size()) {
                R target = segs[row.sel];
                auto it = w->seg.find(row.key);
                std::pair<float, float> cur = it == w->seg.end() ? std::make_pair(target.l, target.r) : it->second;
                cur.first += (target.l - cur.first) * 0.3f;
                cur.second += (target.r - cur.second) * 0.3f;
                if (std::fabs(cur.first - target.l) < 0.4f && std::fabs(cur.second - target.r) < 0.4f)
                    cur = {target.l, target.r};
                else
                    animating = true;
                w->seg[row.key] = cur;
                Round(R(cur.first, target.t, cur.second, target.b), 7, P.accent);
            }
            for (size_t i = 0; i < segs.size(); ++i) {
                bool on = (int)i == row.sel;
                Text(row.opts[i], w->fCtrl, segs[i], on ? P.onAccent : P.text, on ? 1.0f : 0.82f);
                w->hits.push_back({segs[i], {A_CHOOSE, row.key, (int)i}});
            }
            break;
        }
        case Ctl::Slider: {
            const float labelW = 54;
            R track(right - cw, cy - 3, right - labelW - 14, cy + 3);
            float f = (row.value - kThresholdMin) / (float)(kThresholdMax - kThresholdMin);
            float x = track.l + track.w() * f;
            Round(track, 3, P.ctrl);
            Round(R(track.l, track.t, std::max(track.l + 6, x), track.b), 3, P.low, 0.9f);
            for (int v = kThresholdMin; v <= kThresholdMax; v += kThresholdStep) {
                float tx2 = track.l + track.w() * (v - kThresholdMin) / (float)(kThresholdMax - kThresholdMin);
                Circle(tx2, cy + 13, 1.6f, v <= row.value ? P.low : P.dim, 0.7f);
            }
            Act a{A_SLIDER, row.key};
            bool hv = w->hover == a || w->dragKey == row.key;
            Circle(x, cy, hv ? 10.5f : 9.5f, P.card);
            Ring(x, cy, hv ? 10.5f : 9.5f, P.low, 1, 2.5f);
            Circle(x, cy, 3.5f, P.low);
            Text(std::to_wstring(row.value) + L" %", w->fCtrlR, R(right - labelW, cy - 12, right, cy + 12), P.low);
            w->sliderTrack[row.key] = track;
            w->hits.push_back({R(track.l - 12, cy - 18, track.r + 12, cy + 18), a});
            break;
        }
        case Ctl::Text:
            Text(row.right, row.glyph ? w->fIcon : w->fCtrlR, R(right - cw, cy - 14, right, cy + 14), P.dim);
            break;
        case Ctl::Buttons: {
            float by = r.b - 15 - 32, x = tx;
            for (size_t i = 0; i < row.buttons.size(); ++i) {
                float bw = BtnW(row.buttons[i].first);
                Pill(row.buttons[i].first, R(x, by, x + bw, by + 32), {A_CMD, row.buttons[i].second}, i == 0);
                x += bw + 8;
            }
            break;
        }
        default: break;
    }
    g_alpha = saved;
}

// ------------------------------------------------------------------ paint
void ClientDips(float* cw, float* ch) {
    RECT rc;
    GetClientRect(w->hwnd, &rc);
    *cw = rc.right / w->scale;
    *ch = rc.bottom / w->scale;
}

void ClampScroll() {
    float cw, ch;
    ClientDips(&cw, &ch);
    float max = std::max(0.0f, w->contentH - ch + 24);
    w->scroll = std::min(std::max(w->scroll, 0.0f), max);
}

void StartAnim() {
    if (!w->timer) w->timer = SetTimer(w->hwnd, TIMER_ANIM, 15, nullptr) != 0;
}

void DrawBrandBattery(float x, float y, float level) {
    R body(x, y, x + 24, y + 13);
    Outline(body, 3.5f, P.text, 1, 1.6f);
    Round(R(body.r + 1.5f, y + 4, body.r + 3.5f, y + 9), 1, P.text);
    Round(R(x + 3, y + 3, x + 3 + 18 * level, y + 10), 1.5f, P.accent);
}

void Paint() {
    if (!EnsureGfx()) return;
    float W, H;
    ClientDips(&W, &H);
    w->hits.clear();
    w->sliderTrack.clear();
    bool animating = false;
    float t = std::min(1.0f, (GetTickCount64() - w->animStart) / 600.0f);
    float fill = EaseOut(t);
    if (t < 1) animating = true;

    w->rt->BeginDraw();
    w->rt->SetTransform(D2D1::Matrix3x2F::Identity());
    w->rt->Clear(D2D1::ColorF(P.bg.r, P.bg.g, P.bg.b));

    // ---------------- content (scrolls)
    const SectionDef& sec = kSections[w->section];
    float x0 = kSidebarW + kPad;
    float x1 = std::max(std::min(W - kPad, x0 + kContentMaxW), x0 + 360);
    // Sections slide in slightly when opened.
    float slide = (1 - fill) * 14;
    float y = 30 - w->scroll + slide;
    float headR = x1;
    if (w->section == 0) {
        std::wstring label = w->refreshing ? L"Buscando…" : L"Actualizar ahora";
        float bw = BtnW(label) + 22;
        R b(x1 - bw, y + 2, x1, y + 34);
        Act a{A_CMD, C_REFRESH};
        bool hv = w->hover == a;
        Round(b, 16, hv ? P.ctrlHover : P.ctrl);
        Text(L"", w->fIconC, R(b.l + 12, b.t, b.l + 30, b.b), P.accent);
        Text(label, w->fCtrl, R(b.l + 24, b.t, b.r - 4, b.b), P.text);
        if (!w->refreshing) w->hits.push_back({b, a});
        headR = b.l - 12;
    }
    Text(sec.name, w->fTitle, R(x0, y, headR, y + 38), P.text, fill);
    y += 40;
    Text(sec.sub, w->fSub, R(x0, y, x1, y + 20), P.dim, fill);
    y += 36;

    std::vector<Group> groups = BuildSection(w->section);
    for (const Group& g : groups) {
        if (!g.title.empty()) {
            std::wstring up = g.title;
            CharUpperBuffW(&up[0], (DWORD)up.size());
            Text(up, w->fGroup, R(x0 + 4, y, x1, y + 18), P.dim);
            y += 24;
        }
        std::vector<float> hs;
        float cardH = 0;
        for (const Row& r : g.rows) {
            hs.push_back(RowHeight(r, x1 - x0));
            cardH += hs.back();
        }
        R card(x0, y, x1, y + cardH);
        Round(card, 12, P.card);
        Outline(card, 12, P.line, 0.7f, 1);
        for (size_t i = 0; i < g.rows.size(); ++i) {
            if (i > 0) Fill(R(x0 + 18, y, x1 - 18, y + 1), P.line, 0.9f);
            DrawRow(g.rows[i], R(x0, y, x1, y + hs[i]), fill, animating);
            y += hs[i];
        }
        y += 22;
    }
    w->contentH = y + w->scroll - slide;

    // Thin scroll indicator.
    if (w->contentH > H) {
        float th = std::max(40.0f, H * H / (w->contentH + 24));
        float ty = w->scroll / std::max(1.0f, w->contentH + 24 - H) * (H - th);
        Round(R(W - 7, ty + 4, W - 3, ty + th - 4), 2, P.dim, 0.35f);
    }

    // ---------------- sidebar (drawn over the content)
    Fill(R(0, 0, kSidebarW, H), P.side);
    Fill(R(kSidebarW - 1, 0, kSidebarW, H), P.line, 0.8f);
    DrawBrandBattery(24, 30, 0.35f + 0.5f * fill);
    Text(L"BatteryHub", w->fBrand, R(60, 18, kSidebarW - 8, 44), P.text);
    Text(L"Versión " + CurrentVersion(), w->fSmall, R(60, 43, kSidebarW - 8, 61), P.dim);

    float sy = 88;
    for (int i = 0; i < kSectionCount; ++i) {
        R r(12, sy, kSidebarW - 12, sy + 40);
        bool sel = i == w->section;
        Act a{A_SECTION, i};
        if (sel) Round(r, 9, P.accent, 0.16f);
        else if (w->hover == a) Round(r, 9, P.ctrl, 0.7f);
        Text(kSections[i].glyph, w->fIcon, R(r.l + 8, r.t, r.l + 40, r.b), sel ? P.accent : P.dim);
        Text(kSections[i].name, sel ? w->fRowB : w->fRow, R(r.l + 46, r.t, r.r - 8, r.b), P.text, sel ? 1.0f : 0.86f);
        if (i == 0 && !w->devices.empty()) {  // device count badge
            std::wstring cnt = std::to_wstring(w->devices.size());
            float bw = std::max(22.0f, TextW(cnt, w->fCtrl) + 12);
            R badge(r.r - 10 - bw, r.t + 10, r.r - 10, r.b - 10);
            Round(badge, 10, sel ? P.accent : P.ctrl);
            Text(cnt, w->fCtrl, badge, sel ? P.onAccent : P.dim);
        }
        w->hits.push_back({r, a});
        sy += 44;
    }

    // Live summary: the device with the least battery left.
    const DeviceView* lowest = nullptr;
    for (const auto& d : w->devices)
        if (d.percent >= 0 && (!lowest || d.percent < lowest->percent)) lowest = &d;
    if (H > sy + 150) {
        R card(14, H - 132, kSidebarW - 14, H - 46);
        Round(card, 12, P.card);
        Outline(card, 12, P.line, 0.7f, 1);
        if (lowest) {
            C lvl = LevelColor(lowest->percent);
            Text(L"Más baja", w->fSmall, R(card.l + 14, card.t + 10, card.r - 14, card.t + 28), P.dim);
            Text(lowest->name, w->fRowB, R(card.l + 14, card.t + 28, card.r - 60, card.t + 50), P.text);
            Text(Pct(lowest->percent), w->fPct, R(card.r - 64, card.t + 26, card.r - 14, card.t + 52), lvl);
            R bar(card.l + 14, card.t + 62, card.r - 14, card.t + 68);
            Round(bar, 3, P.ctrl);
            Round(R(bar.l, bar.t, bar.l + std::max(6.0f, bar.w() * lowest->percent / 100.0f * fill), bar.b), 3, lvl);
        } else {
            Text(L"Sin lecturas", w->fRowB, R(card.l + 14, card.t + 16, card.r - 14, card.t + 38), P.text);
            Text(L"Conectá un dispositivo", w->fSmall, R(card.l + 14, card.t + 40, card.r - 14, card.t + 58), P.dim);
        }
    }
    Text(L"Los cambios se guardan solos", w->fSmall, R(24, H - 36, kSidebarW - 8, H - 14), P.dim, 0.85f);

    HRESULT hr = w->rt->EndDraw();
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) {
        ReleaseTarget();
        animating = true;  // repaint with a fresh target
    }
    if (animating) StartAnim();
    else if (w->timer) {
        KillTimer(w->hwnd, TIMER_ANIM);
        w->timer = false;
    }
}

void Invalidate() {
    if (w && w->hwnd) InvalidateRect(w->hwnd, nullptr, FALSE);
}

// ------------------------------------------------------------------ input
Act HitAt(float x, float y) {
    for (auto it = w->hits.rbegin(); it != w->hits.rend(); ++it)
        if (it->first.Has(x, y)) return it->second;
    return {};
}

void SetSliderFromX(float x) {
    auto it = w->sliderTrack.find(w->dragKey);
    if (it == w->sliderTrack.end()) return;
    const R& tr = it->second;
    float f = std::min(1.0f, std::max(0.0f, (x - tr.l) / tr.w()));
    int steps = (int)std::lround(f * (kThresholdMax - kThresholdMin) / kThresholdStep);
    int v = kThresholdMin + steps * kThresholdStep;
    Settings& st = w->host->Prefs();
    if (v != st.lowThreshold) {
        st.lowThreshold = v;
        w->host->Changed(SettingsHost::Display);
    }
    Invalidate();
}

void OpenUrl(const wchar_t* url) { ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL); }

void Do(const Act& a) {
    Settings& st = w->host->Prefs();
    SettingsHost* host = w->host;
    switch (a.t) {
        case A_SECTION:
            if (a.a != w->section) {
                w->section = a.a;
                w->scroll = 0;
                w->animStart = GetTickCount64();
                w->seg.clear();
                w->knob.clear();
            }
            break;
        case A_TOGGLE:
            switch (a.a) {
                case K_HIDE:
                    st.hideDisconnected = !st.hideDisconnected;
                    host->Changed(SettingsHost::Display);
                    break;
                case K_GAME:
                    st.gameMode = !st.gameMode;
                    host->Changed(SettingsHost::General);
                    break;
                case K_GAME_BROWSERS:
                    st.gameModeBrowsers = !st.gameModeBrowsers;
                    host->Changed(SettingsHost::General);
                    break;
                case K_AUTOSTART: host->SetAutostart(!host->Autostart()); break;
            }
            break;
        case A_CHOOSE:
            switch (a.a) {
                case K_MODE:
                    st.trayIcons = a.b == 2;
                    if (a.b != 2) st.widgetLeft = a.b == 0;
                    host->Changed(SettingsHost::Display);
                    break;
                case K_THEME:
                    st.menuTheme = a.b;
                    host->Changed(SettingsHost::General);
                    ApplyTheme();
                    break;
                case K_INTERVAL:
                    st.intervalSec = kIntervals[a.b];
                    host->Changed(SettingsHost::Interval);
                    break;
            }
            break;
        case A_CMD:
            switch (a.a) {
                case C_REFRESH:
                    w->refreshing = true;
                    host->RefreshNow();
                    break;
                case C_CHECK: host->CheckUpdate(); break;
                case C_INSTALL: host->InstallUpdate(); break;
                case C_RELEASES: OpenUrl(kReleasesPage); break;
                case C_REPO: OpenUrl(L"https://github.com/Maurito2004/BatteryHub"); break;
                case C_ISSUES: OpenUrl(L"https://github.com/Maurito2004/BatteryHub/issues/new"); break;
            }
            break;
        case A_RENAME:
        case A_UP:
        case A_DOWN: {
            if (a.a < 0 || a.a >= (int)w->devices.size()) break;
            std::wstring id = w->devices[a.a].id;
            if (a.t == A_RENAME) host->Rename(id);
            else host->Move(id, a.t == A_UP ? -1 : 1);
            if (!w) return;  // closed while the rename dialog was up
            w->devices = host->Devices();
            break;
        }
    }
    if (w) {
        w->hover = {};
        Invalidate();
    }
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (!w) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
        case WM_NCCREATE:
            w->hwnd = hwnd;
            break;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            Paint();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (w->rt) w->rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            ClampScroll();
            Invalidate();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize.x = (LONG)(780 * w->scale);
            mm->ptMinTrackSize.y = (LONG)(500 * w->scale);
            return 0;
        }
        case WM_DPICHANGED: {
            w->scale = HIWORD(wp) / 96.0f;
            ReleaseTarget();
            const RECT* s = (const RECT*)lp;
            SetWindowPos(hwnd, nullptr, s->left, s->top, s->right - s->left, s->bottom - s->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            Invalidate();
            return 0;
        }
        case WM_MOUSEMOVE: {
            float x = GET_X_LPARAM(lp) / w->scale, y = GET_Y_LPARAM(lp) / w->scale;
            if (w->dragKey) {
                SetSliderFromX(x);
                return 0;
            }
            Act a = HitAt(x, y);
            if (!(a == w->hover)) {
                w->hover = a;
                Invalidate();
            }
            if (!w->tracking) {
                TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
                w->tracking = TrackMouseEvent(&tme) != FALSE;
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            w->tracking = false;
            if (!w->hover.None()) {
                w->hover = {};
                Invalidate();
            }
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, w->hover.None() && !w->dragKey ? IDC_ARROW : IDC_HAND));
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN: {
            float x = GET_X_LPARAM(lp) / w->scale, y = GET_Y_LPARAM(lp) / w->scale;
            Act a = HitAt(x, y);
            if (a.t == A_SLIDER) {
                w->dragKey = a.a;
                SetCapture(hwnd);
                SetSliderFromX(x);
            } else {
                w->pressed = a;
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (w->dragKey) {
                w->dragKey = 0;
                ReleaseCapture();
                Invalidate();
                return 0;
            }
            Act a = HitAt(GET_X_LPARAM(lp) / w->scale, GET_Y_LPARAM(lp) / w->scale);
            Act pressed = w->pressed;
            w->pressed = {};
            if (!a.None() && a == pressed) Do(a);
            return 0;
        }
        case WM_CAPTURECHANGED:
            w->dragKey = 0;
            return 0;
        case WM_MOUSEWHEEL:
            w->scroll -= GET_WHEEL_DELTA_WPARAM(wp) / 120.0f * 56.0f;
            ClampScroll();
            Invalidate();
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_TIMER:
            if (wp == TIMER_ANIM) Invalidate();
            return 0;
        case WM_SETTINGCHANGE:
            if (lp && lstrcmpiW((LPCWSTR)lp, L"ImmersiveColorSet") == 0) {
                ApplyTheme();
                Invalidate();
            }
            return 0;
        case WM_DESTROY: {
            if (w->timer) KillTimer(hwnd, TIMER_ANIM);
            ReleaseAll();
            Win* dead = w;
            w = nullptr;
            delete dead;
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

void OpenSettingsWindow(HINSTANCE inst, SettingsHost* host) {
    if (w) {
        if (IsIconic(w->hwnd)) ShowWindow(w->hwnd, SW_RESTORE);
        SetForegroundWindow(w->hwnd);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = Proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
        wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                       GetSystemMetrics(SM_CYSMICON), 0);
        wc.lpszClassName = kClass;
        registered = RegisterClassExW(&wc) != 0;
    }
    w = new Win;
    w->inst = inst;
    w->host = host;
    w->devices = host->Devices();
    w->dark = IsDark(host->Prefs().menuTheme);
    P = MakePal(w->dark);

    // Centered on the monitor under the mouse (the one the user is looking at).
    POINT cur;
    GetCursorPos(&cur);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(cur, MONITOR_DEFAULTTOPRIMARY), &mi);
    HWND hwnd = CreateWindowExW(0, kClass, L"Configuración de BatteryHub", WS_OVERLAPPEDWINDOW, mi.rcWork.left,
                                mi.rcWork.top, 100, 100, nullptr, nullptr, inst, nullptr);
    if (!hwnd || !w) {
        delete w;
        w = nullptr;
        return;
    }
    w->hwnd = hwnd;
    using DpiFn = UINT(WINAPI*)(HWND);
    static auto getDpi = (DpiFn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    w->scale = (getDpi ? std::max(96u, getDpi(hwnd)) : 96u) / 96.0f;
    SetTitleBar(hwnd, w->dark);
    const RECT& wa = mi.rcWork;
    int ww = std::min((int)(860 * w->scale), (int)(wa.right - wa.left));
    int wh = std::min((int)(620 * w->scale), (int)(wa.bottom - wa.top));
    SetWindowPos(hwnd, nullptr, wa.left + (wa.right - wa.left - ww) / 2, wa.top + (wa.bottom - wa.top - wh) / 2, ww,
                 wh, SWP_NOZORDER);
    w->animStart = GetTickCount64();
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
}

void RefreshSettingsWindow() {
    if (!w) return;
    w->devices = w->host->Devices();
    w->refreshing = false;
    Invalidate();
}

void CloseSettingsWindow() {
    if (w) DestroyWindow(w->hwnd);
}

bool SettingsWindowOpen() { return w != nullptr; }

HWND SettingsWindowHandle() { return w ? w->hwnd : nullptr; }

}  // namespace bh
