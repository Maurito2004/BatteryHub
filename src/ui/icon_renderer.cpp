// GDI+ rendering. Device glyphs come from the system icon font (Segoe Fluent
// Icons on Windows 11, Segoe MDL2 Assets on Windows 10) so they match the
// native taskbar style and stay crisp at every DPI without shipping bitmaps.
#include "icon_renderer.h"

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <cstdio>
#include <memory>

#ifdef _MSC_VER
#pragma comment(lib, "gdiplus.lib")
#endif

namespace bh {

using namespace Gdiplus;

namespace {

ULONG_PTR g_token = 0;
std::unique_ptr<FontFamily> g_iconFamily;
std::unique_ptr<FontFamily> g_textFamily;

const wchar_t* GlyphFor(DeviceKind k, bool placeholder) {
    if (placeholder) return L"";  // Battery
    switch (k) {
        case DeviceKind::Headset:  return L"";  // Headphone
        case DeviceKind::Mouse:    return L"";  // Mouse
        case DeviceKind::Keyboard: return L"";  // KeyboardClassic
        case DeviceKind::Gamepad:  return L"";  // Game
        default:                   return L"";  // Devices
    }
}

// Draws `text` centred in `box`, shrinking horizontally if it does not fit.
void DrawFitted(Graphics& g, const wchar_t* text, const Font& font, const Brush& brush, const RectF& box) {
    StringFormat fmt(StringFormat::GenericTypographic());
    fmt.SetFormatFlags(fmt.GetFormatFlags() | StringFormatFlagsNoWrap | StringFormatFlagsNoClip);
    RectF bounds;
    g.MeasureString(text, -1, &font, PointF(0, 0), &fmt, &bounds);
    REAL sx = bounds.Width > box.Width ? box.Width / bounds.Width : 1.0f;
    REAL w = bounds.Width * sx;
    REAL x = box.X + (box.Width - w) / 2;
    REAL y = box.Y + (box.Height - bounds.Height) / 2;
    GraphicsState st = g.Save();
    g.TranslateTransform(x, y);
    g.ScaleTransform(sx, 1.0f);
    g.DrawString(text, -1, &font, PointF(0, 0), &fmt, &brush);
    g.Restore(st);
}

}  // namespace

bool IconRendererInit() {
    GdiplusStartupInput in;
    if (GdiplusStartup(&g_token, &in, nullptr) != Ok) return false;
    for (const wchar_t* name : {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}) {
        auto f = std::make_unique<FontFamily>(name);
        if (f->GetLastStatus() == Ok && f->IsAvailable()) {
            g_iconFamily = std::move(f);
            break;
        }
    }
    g_textFamily = std::make_unique<FontFamily>(L"Segoe UI");
    if (g_textFamily->GetLastStatus() != Ok) g_textFamily = std::make_unique<FontFamily>(L"Arial");
    return true;
}

void IconRendererShutdown() {
    g_iconFamily.reset();
    g_textFamily.reset();
    if (g_token) GdiplusShutdown(g_token);
    g_token = 0;
}

HICON RenderTrayIcon(const IconSpec& s) {
    const int S = s.size < 16 ? 16 : s.size;
    Bitmap bmp(S, S, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.Clear(Color(0, 0, 0, 0));
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);  // no ClearType on transparent bg

    BYTE a = s.stale ? 105 : 255;
    Color ink = s.light ? Color(a, 0, 0, 0) : Color(a, 255, 255, 255);
    if (s.low && !s.stale) ink = s.light ? Color(255, 196, 43, 28) : Color(255, 255, 92, 80);
    if (s.stale) ink = s.light ? Color(a, 130, 130, 130) : Color(a, 200, 200, 200);
    SolidBrush brush(ink);

    const bool charging = s.charge == Charge::Charging || s.charge == Charge::Full;  // bolt = on power
    const REAL topH = S * 0.47f;

    // Top row: device glyph (+ lightning bolt when charging).
    if (g_iconFamily) {
        Font glyph(g_iconFamily.get(), topH * 0.98f, FontStyleRegular, UnitPixel);
        if (charging) {
            DrawFitted(g, GlyphFor(s.kind, s.placeholder), glyph, brush, RectF(0, 0, S * 0.62f, topH));
            Font bolt(g_iconFamily.get(), topH * 0.9f, FontStyleRegular, UnitPixel);
            SolidBrush boltBrush(s.stale ? ink : (s.light ? Color(255, 16, 124, 16) : Color(255, 108, 203, 95)));
            DrawFitted(g, L"", bolt, boltBrush, RectF(S * 0.56f, 0, S * 0.44f, topH));
        } else {
            DrawFitted(g, GlyphFor(s.kind, s.placeholder), glyph, brush, RectF(0, 0, (REAL)S, topH));
        }
    }

    // Bottom: percentage digits, as large as the remaining space allows.
    wchar_t text[8];
    if (s.placeholder || s.percent < 0) wcscpy(text, L"--");
    else _snwprintf(text, 8, L"%d", s.percent > 100 ? 100 : s.percent);
    const REAL textH = S - topH + S * 0.06f;
    Font digits(g_textFamily.get(), textH * 0.98f, FontStyleBold, UnitPixel);
    DrawFitted(g, text, digits, brush, RectF(-0.5f, topH - S * 0.06f, S + 1.0f, textH));

    // Build the icon from a 32-bpp DIB with straight alpha: Bitmap::GetHICON
    // mishandles partially transparent dark pixels (dimmed icons on light taskbars).
    BitmapData data;
    Rect all(0, 0, S, S);
    if (bmp.LockBits(&all, ImageLockModeRead, PixelFormat32bppARGB, &data) != Ok) return nullptr;
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = S;
    bi.bV5Height = -S;  // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color) {
        bmp.UnlockBits(&data);
        return nullptr;
    }
    for (int y = 0; y < S; ++y)
        memcpy((BYTE*)bits + y * S * 4, (BYTE*)data.Scan0 + y * data.Stride, S * 4);
    bmp.UnlockBits(&data);
    HBITMAP mask = CreateBitmap(S, S, 1, 1, nullptr);
    ICONINFO ii = {TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

namespace {

Color InkFor(const IconSpec& s) {
    if (s.stale) return s.light ? Color(140, 110, 110, 110) : Color(115, 170, 170, 170);
    if (s.low) return s.light ? Color(255, 196, 43, 28) : Color(255, 255, 92, 80);
    return s.light ? Color(255, 0, 0, 0) : Color(255, 255, 255, 255);
}

HBITMAP ToPremultipliedDib(Bitmap& bmp, int w, int h) {
    BitmapData data;
    Rect all(0, 0, w, h);
    if (bmp.LockBits(&all, ImageLockModeRead, PixelFormat32bppPARGB, &data) != Ok) return nullptr;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib)
        for (int y = 0; y < h; ++y) memcpy((BYTE*)bits + y * w * 4, (BYTE*)data.Scan0 + y * data.Stride, w * 4);
    bmp.UnlockBits(&data);
    return dib;
}

}  // namespace

const wchar_t* DeviceGlyph(DeviceKind kind) { return GlyphFor(kind, false); }

HBITMAP RenderMenuGlyph(const wchar_t* glyph, int size, bool darkMenu) {
    if (!g_iconFamily || size < 8) return nullptr;
    Bitmap bmp(size, size, PixelFormat32bppPARGB);
    Graphics g(&bmp);
    g.Clear(Color(0, 0, 0, 0));
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    Font font(g_iconFamily.get(), size * 0.86f, FontStyleRegular, UnitPixel);
    SolidBrush ink(darkMenu ? Color(255, 255, 255, 255) : Color(255, 32, 32, 32));
    DrawFitted(g, glyph, font, ink, RectF(0, 0, (REAL)size, (REAL)size));
    return ToPremultipliedDib(bmp, size, size);
}

HBITMAP RenderTaskbarStrip(const std::vector<IconSpec>& items, int height, int dpi, int* outWidth,
                           std::vector<int>* itemRight) {
    const REAL k = dpi / 96.0f;
    const REAL glyphPx = 17 * k, textPx = 14 * k, padX = 7 * k, gap = 5 * k, badgeW = 7 * k;
    StringFormat fmt(StringFormat::GenericTypographic());
    fmt.SetFormatFlags(fmt.GetFormatFlags() | StringFormatFlagsNoWrap | StringFormatFlagsMeasureTrailingSpaces);
    Font text(g_textFamily.get(), textPx, FontStyleRegular, UnitPixel);
    Font badgeFont(g_textFamily.get(), 10 * k, FontStyleBold, UnitPixel);
    std::unique_ptr<Font> glyph, bolt;
    if (g_iconFamily) {
        glyph = std::make_unique<Font>(g_iconFamily.get(), glyphPx, FontStyleRegular, UnitPixel);
        bolt = std::make_unique<Font>(g_iconFamily.get(), glyphPx * 0.62f, FontStyleRegular, UnitPixel);
    }

    // Measure.
    Bitmap probe(1, 1, PixelFormat32bppPARGB);
    Graphics pg(&probe);
    std::vector<std::wstring> labels;
    std::vector<REAL> widths;
    REAL total = 0;
    // Every label gets the width of the widest one ("100%"), so the strip does not
    // shift sideways when a value changes length (100% -> 99%, 9% -> 10%, "--").
    // The number stays next to its glyph; the reserved space is at the end.
    RectF widest;
    pg.MeasureString(L"100%", -1, &text, PointF(0, 0), &fmt, &widest);
    for (const auto& s : items) {
        std::wstring label = (s.placeholder || s.percent < 0) ? L"--" : Fmt(L"%d%%", s.percent > 100 ? 100 : s.percent);
        REAL w = padX + glyphPx + (s.badge ? badgeW : 0) + gap + widest.Width + padX;
        labels.push_back(label);
        widths.push_back(w);
        total += w;
    }
    int W = (int)(total + 0.99f);
    if (W < 1) W = 1;
    *outWidth = W;

    Bitmap bmp(W, height, PixelFormat32bppPARGB);
    Graphics g(&bmp);
    // Alpha 1 everywhere so the whole strip receives mouse clicks yet looks transparent.
    g.Clear(Color(1, 0, 0, 0));
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);

    REAL x = 0;
    itemRight->clear();
    for (size_t i = 0; i < items.size(); ++i) {
        const IconSpec& s = items[i];
        SolidBrush ink(InkFor(s));
        REAL gx = x + padX;
        if (s.highlight) {  // item being dragged: subtle rounded plate behind it
            SolidBrush plate(s.light ? Color(40, 0, 0, 0) : Color(48, 255, 255, 255));
            REAL px = x + 2 * k, py = height * 0.14f, pw = widths[i] - 4 * k, ph = height * 0.72f, rr = 5 * k;
            GraphicsPath path;
            path.AddArc(px, py, 2 * rr, 2 * rr, 180, 90);
            path.AddArc(px + pw - 2 * rr, py, 2 * rr, 2 * rr, 270, 90);
            path.AddArc(px + pw - 2 * rr, py + ph - 2 * rr, 2 * rr, 2 * rr, 0, 90);
            path.AddArc(px, py + ph - 2 * rr, 2 * rr, 2 * rr, 90, 90);
            path.CloseFigure();
            g.FillPath(&plate, &path);
        }
        if (glyph) {
            RectF gb;
            g.MeasureString(GlyphFor(s.kind, s.placeholder), -1, glyph.get(), PointF(0, 0), &fmt, &gb);
            g.DrawString(GlyphFor(s.kind, s.placeholder), -1, glyph.get(),
                         PointF(gx + (glyphPx - gb.Width) / 2, (height - gb.Height) / 2), &fmt, &ink);
            if ((s.charge == Charge::Charging || s.charge == Charge::Full) && !s.stale) {  // on power
                SolidBrush green(s.light ? Color(255, 16, 124, 16) : Color(255, 108, 203, 95));
                RectF bb;
                g.MeasureString(L"", -1, bolt.get(), PointF(0, 0), &fmt, &bb);
                g.DrawString(L"", -1, bolt.get(),
                             PointF(gx + glyphPx - bb.Width * 0.55f, height / 2.0f + glyphPx * 0.5f - bb.Height),
                             &fmt, &green);
            }
        }
        REAL textX = gx + glyphPx + gap;
        if (s.badge) {  // controller number, subscript-style after the glyph
            wchar_t num[4];
            _snwprintf(num, 4, L"%d", s.badge);
            g.DrawString(num, -1, &badgeFont, PointF(gx + glyphPx + k, height / 2.0f + k), &fmt, &ink);
            textX += badgeW;
        }
        RectF tb;
        g.MeasureString(labels[i].c_str(), -1, &text, PointF(0, 0), &fmt, &tb);
        g.DrawString(labels[i].c_str(), -1, &text, PointF(textX, (height - tb.Height) / 2 - k), &fmt, &ink);
        x += widths[i];
        itemRight->push_back((int)(x + 0.5f));
    }
    return ToPremultipliedDib(bmp, W, height);
}

// Dev aid (`--preview file.png`): every state at 16/24/32 px on dark and light taskbars.
bool RenderPreviewPng(const wchar_t* path) {
    struct Sample { DeviceKind k; int pct; Charge c; bool stale; };
    const Sample samples[] = {{DeviceKind::Headset, 30, Charge::Discharging, false},
                              {DeviceKind::Mouse, 93, Charge::Discharging, false},
                              {DeviceKind::Keyboard, 100, Charge::Full, false},
                              {DeviceKind::Gamepad, 55, Charge::Charging, false},
                              {DeviceKind::Mouse, 12, Charge::Discharging, false},
                              {DeviceKind::Headset, 64, Charge::Unknown, true},
                              {DeviceKind::Unknown, -1, Charge::Unknown, false}};
    const int sizes[] = {16, 24, 32};
    const int cell = 40, n = (int)(sizeof(samples) / sizeof(samples[0]));
    // Plain GDI: opaque background + DrawIconEx, the way Explorer composites tray icons.
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP hbm = CreateCompatibleBitmap(screen, cell * n, cell * 6);
    ReleaseDC(nullptr, screen);
    HGDIOBJ oldBm = SelectObject(dc, hbm);
    for (int theme = 0; theme < 2; ++theme) {
        RECT rc = {0, theme * cell * 3, cell * n, (theme + 1) * cell * 3};
        HBRUSH bg = CreateSolidBrush(theme ? RGB(238, 238, 238) : RGB(32, 32, 32));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        for (int si = 0; si < 3; ++si)
            for (int i = 0; i < n; ++i) {
                IconSpec s;
                s.kind = samples[i].k;
                s.percent = samples[i].pct;
                s.charge = samples[i].c;
                s.stale = samples[i].stale;
                s.low = s.percent >= 0 && s.percent <= 15;
                s.light = theme == 1;
                s.size = sizes[si];
                HICON ic = RenderTrayIcon(s);
                DrawIconEx(dc, i * cell + (cell - s.size) / 2, (theme * 3 + si) * cell + (cell - s.size) / 2, ic,
                           s.size, s.size, 0, nullptr, DI_NORMAL);
                DestroyIcon(ic);
            }
    }
    SelectObject(dc, oldBm);
    DeleteDC(dc);
    // PNG encoder CLSID {557CF406-1A04-11D3-9A73-0000F81EF32E}
    const CLSID png = {0x557CF406, 0x1A04, 0x11D3, {0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E}};
    bool ok;
    {
        Bitmap sheet(hbm, nullptr);
        ok = sheet.Save(path, &png, nullptr) == Ok;
    }
    DeleteObject(hbm);
    return ok;
}

}  // namespace bh
