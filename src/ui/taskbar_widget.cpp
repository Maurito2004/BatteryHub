#include "taskbar_widget.h"

#include <commctrl.h>

#include <cstdlib>

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#endif

namespace bh {

namespace {
constexpr wchar_t kWidgetClass[] = L"BatteryHub.TaskbarWidget";

int DpiOf(HWND hwnd) {
    using Fn = UINT(WINAPI*)(HWND);
    static Fn getDpiForWindow = (Fn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (getDpiForWindow && hwnd) return (int)getDpiForWindow(hwnd);
    HDC dc = GetDC(nullptr);
    int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    return dpi;
}
}  // namespace

bool TaskbarWidget::Create(HINSTANCE inst, HWND owner, UINT menuMsg, UINT reorderMsg) {
    owner_ = owner;
    menuMsg_ = menuMsg;
    reorderMsg_ = reorderMsg;
    taskbar_ = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!taskbar_ || !FindWindowExW(taskbar_, nullptr, L"TrayNotifyWnd", nullptr)) return false;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kWidgetClass;
        RegisterClassExW(&wc);
        INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_WIN95_CLASSES};
        InitCommonControlsEx(&icc);
        registered = true;
    }
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kWidgetClass, L"BatteryHub",
                            WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, this);
    if (!hwnd_) return false;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);
    // Become a child of the taskbar: moves, hides and stacks with it.
    SetParent(hwnd_, taskbar_);
    LONG style = GetWindowLongW(hwnd_, GWL_STYLE);
    SetWindowLongW(hwnd_, GWL_STYLE, (style & ~WS_POPUP) | WS_CHILD | WS_CLIPSIBLINGS);

    tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd_, nullptr, inst,
                               nullptr);
    if (tooltip_) SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, 400);  // enables multi-line tips
    dpi_ = DpiOf(taskbar_);
    return true;
}

void TaskbarWidget::Destroy() {
    if (tooltip_) DestroyWindow(tooltip_);
    if (hwnd_) DestroyWindow(hwnd_);
    tooltip_ = hwnd_ = nullptr;
    items_.clear();
}

std::vector<std::wstring> TaskbarWidget::Order() const {
    std::vector<std::wstring> ids;
    for (const auto& it : items_) ids.push_back(it.id);
    return ids;
}

int TaskbarWidget::HitIndex(int x) const {
    for (size_t i = 0; i < itemRight_.size(); ++i)
        if (x < itemRight_[i]) return (int)i;
    return itemRight_.empty() ? -1 : (int)itemRight_.size() - 1;
}

// Press-and-drag reorders the items live; a press released without moving is a click.
void TaskbarWidget::OnMouse(UINT msg, int x) {
    if (msg == WM_LBUTTONDOWN) {
        pressIndex_ = HitIndex(x);
        pressX_ = x;
        dragging_ = false;
        SetCapture(hwnd_);
        return;
    }
    if (msg == WM_MOUSEMOVE) {
        if (pressIndex_ < 0 || GetCapture() != hwnd_) return;
        if (!dragging_ && abs(x - pressX_) >= GetSystemMetrics(SM_CXDRAG)) {
            dragging_ = true;
            items_[pressIndex_].spec.highlight = true;
            Render();
        }
        if (!dragging_) return;
        int target = HitIndex(x);
        if (target >= 0 && target != pressIndex_) {
            WidgetItem moved = items_[pressIndex_];
            items_.erase(items_.begin() + pressIndex_);
            items_.insert(items_.begin() + target, moved);
            pressIndex_ = target;
            Render();
        }
        return;
    }
    if (msg == WM_LBUTTONUP || msg == WM_CAPTURECHANGED) {
        bool wasDrag = dragging_, wasPress = pressIndex_ >= 0;
        if (pressIndex_ >= 0 && pressIndex_ < (int)items_.size()) items_[pressIndex_].spec.highlight = false;
        pressIndex_ = -1;
        dragging_ = false;
        if (msg == WM_LBUTTONUP && GetCapture() == hwnd_) ReleaseCapture();
        if (wasDrag) {
            Render();
            RebuildTooltips();
            PostMessageW(owner_, reorderMsg_, 0, 0);
        } else if (wasPress && msg == WM_LBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            PostMessageW(owner_, menuMsg_, (WPARAM)pt.x, (LPARAM)pt.y);
        }
    }
}

void TaskbarWidget::Update(const std::vector<WidgetItem>& items) {
    if (!hwnd_) return;
    if (dragging_) return;  // don't reshuffle under the user's finger; next refresh applies
    items_ = items;
    dpi_ = DpiOf(taskbar_);
    RECT tb;
    GetWindowRect(taskbar_, &tb);
    height_ = tb.bottom - tb.top;
    Render();
    lastRect_ = {};  // force re-anchoring (width may have changed)
    Reposition();
    RebuildTooltips();
}

void TaskbarWidget::Render() {
    std::vector<IconSpec> specs;
    for (const auto& it : items_) specs.push_back(it.spec);
    int w = 0;
    HBITMAP dib = RenderTaskbarStrip(specs, height_ > 0 ? height_ : 48, dpi_, &w, &itemRight_);
    if (!dib) return;
    width_ = w;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, dib);
    SIZE size = {width_, height_};
    POINT src = {0, 0};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, screen, nullptr, &size, mem, &src, 0, &blend, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    DeleteObject(dib);
}

bool TaskbarWidget::Reposition() {
    if (!hwnd_) return false;
    if (!IsWindow(taskbar_)) return false;  // Explorer restarted: caller recreates us
    RECT tb, notify;
    HWND n = FindWindowExW(taskbar_, nullptr, L"TrayNotifyWnd", nullptr);
    if (!n || !GetWindowRect(taskbar_, &tb) || !GetWindowRect(n, &notify)) return false;
    if (tb.right - tb.left <= tb.bottom - tb.top) {  // vertical taskbar: not supported
        ShowWindow(hwnd_, SW_HIDE);
        return false;
    }
    if (tb.bottom - tb.top != height_) {  // taskbar resized (DPI, small icons)
        height_ = tb.bottom - tb.top;
        Render();
    }
    int margin = MulDiv(4, dpi_, 96);
    // Occupied middle area: Start button + search + app buttons.
    LONG appsLeft = notify.left, appsRight = tb.left;
    for (const wchar_t* cls : {L"Start", L"ReBarWindow32"}) {
        RECT r;
        HWND w = FindWindowExW(taskbar_, nullptr, cls, nullptr);
        if (w && GetWindowRect(w, &r) && r.right > r.left) {
            appsLeft = r.left < appsLeft ? r.left : appsLeft;
            appsRight = r.right > appsRight ? r.right : appsRight;
        }
    }
    LONG leftX = tb.left + MulDiv(12, dpi_, 96);
    LONG rightX = notify.left - width_ - margin;
    bool leftFits = leftX + width_ + margin <= appsLeft;
    bool rightFits = rightX >= appsRight + margin;
    LONG x;
    if (anchorLeft_) x = (leftFits || !rightFits) ? leftX : rightX;
    else x = (rightFits || !leftFits) ? rightX : leftX;
    POINT p = {x, tb.top};
    ScreenToClient(taskbar_, &p);
    RECT want = {p.x, p.y, p.x + width_, p.y + height_};
    if (!EqualRect(&want, &lastRect_) || !IsWindowVisible(hwnd_)) {
        SetWindowPos(hwnd_, HWND_TOP, want.left, want.top, width_, height_, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        lastRect_ = want;
    } else {
        // Keep above the XAML taskbar content, which can be re-stacked by Explorer.
        SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    return true;
}

void TaskbarWidget::RebuildTooltips() {
    if (!tooltip_) return;
    int count = (int)SendMessageW(tooltip_, TTM_GETTOOLCOUNT, 0, 0);
    for (int i = 0; i < count; ++i) {
        TOOLINFOW ti = {sizeof(ti)};
        ti.hwnd = hwnd_;
        ti.uId = (UINT_PTR)i;
        SendMessageW(tooltip_, TTM_DELTOOL, 0, (LPARAM)&ti);
    }
    int left = 0;
    for (size_t i = 0; i < items_.size() && i < itemRight_.size(); ++i) {
        TOOLINFOW ti = {sizeof(ti)};
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = hwnd_;
        ti.uId = (UINT_PTR)i;
        ti.rect = {left, 0, itemRight_[i], height_};
        ti.lpszText = (LPWSTR)items_[i].tip.c_str();
        SendMessageW(tooltip_, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        left = itemRight_[i];
    }
}

LRESULT CALLBACK TaskbarWidget::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = (TaskbarWidget*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_LBUTTONDOWN:
        case WM_MOUSEMOVE:
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if (self) self->OnMouse(msg, (short)LOWORD(lp));
            return 0;
        case WM_RBUTTONUP:
            if (self) {
                POINT pt;
                GetCursorPos(&pt);
                PostMessageW(self->owner_, self->menuMsg_, (WPARAM)pt.x, (LPARAM)pt.y);
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace bh
