// Inline taskbar display ("[glyph] 93%" per device), embedded into the
// taskbar just left of the notification area — like the original BatteryHub.
// A layered child window of Shell_TrayWnd: no own taskbar button, follows the
// taskbar (auto-hide, full-screen), transparent background.
#pragma once
#include <windows.h>

#include <string>
#include <vector>

#include "icon_renderer.h"

namespace bh {

struct WidgetItem {
    std::wstring id;
    IconSpec spec;
    std::wstring tip;
};

class TaskbarWidget {
public:
    // menuMsg is posted to `owner` with (x, y) screen coords on any click.
    bool Create(HINSTANCE inst, HWND owner, UINT menuMsg);
    void Destroy();
    bool Ok() const { return hwnd_ != nullptr; }

    void Update(const std::vector<WidgetItem>& items);
    // true = left end of the taskbar (default), false = next to the clock. If the
    // preferred side would overlap the Start button / app buttons, the other side is used.
    void SetAnchorLeft(bool left) { anchorLeft_ = left; lastRect_ = {}; }
    // Game mode: hidden until the next Reposition()/Update().
    void Hide() {
        if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
        lastRect_ = {};
    }
    // Re-anchors next to the notification area; false if the taskbar is gone or vertical.
    bool Reposition();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void Render();
    void RebuildTooltips();

    HWND hwnd_ = nullptr, taskbar_ = nullptr, tooltip_ = nullptr, owner_ = nullptr;
    UINT menuMsg_ = 0;
    std::vector<WidgetItem> items_;
    std::vector<int> itemRight_;
    int width_ = 0, height_ = 0, dpi_ = 96;
    RECT lastRect_ = {};
    bool anchorLeft_ = true;
};

}  // namespace bh
