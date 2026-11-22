// Draws the dynamic tray icon: device glyph on top, percentage below.
#pragma once
#include <windows.h>

#include <vector>

#include "../core/device.h"

namespace bh {

struct IconSpec {
    DeviceKind kind = DeviceKind::Unknown;
    int percent = -1;
    Charge charge = Charge::Unknown;
    bool stale = false;        // dimmed: value is not live
    bool low = false;          // red
    bool light = false;        // light taskbar -> dark ink
    bool placeholder = false;  // "no devices" icon
    int size = 16;             // pixels (small icon metric at the taskbar DPI)

    bool operator==(const IconSpec& o) const {
        return kind == o.kind && percent == o.percent && charge == o.charge && stale == o.stale &&
               low == o.low && light == o.light && placeholder == o.placeholder && size == o.size;
    }
    bool operator!=(const IconSpec& o) const { return !(*this == o); }
};

bool IconRendererInit();
void IconRendererShutdown();
HICON RenderTrayIcon(const IconSpec& spec);  // caller owns the HICON (DestroyIcon)
bool RenderPreviewPng(const wchar_t* path);  // dev aid: --preview <file.png>

// Taskbar strip: "[glyph] 93%" per item, laid out left to right, `height` px tall.
// Returns a premultiplied 32-bpp top-down DIB (for UpdateLayeredWindow), its width,
// and the right edge of every item (for hit-testing / tooltips).
HBITMAP RenderTaskbarStrip(const std::vector<IconSpec>& items, int height, int dpi, int* outWidth,
                           std::vector<int>* itemRight);

}  // namespace bh
