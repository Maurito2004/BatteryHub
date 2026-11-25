// Settings window: a normal top-level window drawn with Direct2D/DirectWrite.
// It only exists while open; closing it frees the window, the render target and
// the fonts, so at rest it costs nothing. The app plugs in through SettingsHost.
#pragma once
#include <windows.h>

#include <string>
#include <vector>

#include "../core/device.h"

namespace bh {

struct Settings;

// One device as the settings window shows it (already in the user's order).
struct DeviceView {
    std::wstring id;
    std::wstring name;     // alias or reported name
    std::wstring detail;   // "Bluetooth · Cargando"
    DeviceKind kind = DeviceKind::Unknown;
    int percent = -1;
    bool charging = false;
    bool stale = false;    // asleep: last known value
};

struct UpdateView {
    enum State { Unknown, Checking, UpToDate, Available, Installing, Failed } state = Unknown;
    std::wstring latest;
};

class SettingsHost {
public:
    virtual ~SettingsHost() = default;
    virtual Settings& Prefs() = 0;
    virtual std::vector<DeviceView> Devices() = 0;
    // Something in Prefs() changed: save and apply it.
    enum Change { General, Interval, Display };
    virtual void Changed(Change what) = 0;
    virtual void Rename(const std::wstring& id) = 0;
    virtual void Move(const std::wstring& id, int delta) = 0;  // -1 earlier, +1 later
    virtual void RefreshNow() = 0;
    virtual bool Autostart() = 0;
    virtual void SetAutostart(bool on) = 0;
    virtual UpdateView Update() = 0;
    virtual void CheckUpdate() = 0;
    virtual void InstallUpdate() = 0;
};

// Opens the window (or brings it to the front when it is already open).
void OpenSettingsWindow(HINSTANCE inst, SettingsHost* host);
// Repaint with fresh data (after a poll, an update check, a rename...).
void RefreshSettingsWindow();
void CloseSettingsWindow();
bool SettingsWindowOpen();
HWND SettingsWindowHandle();  // nullptr when closed

}  // namespace bh
