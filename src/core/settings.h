// Settings live in HKCU\Software\BatteryHub (no files next to the portable exe,
// no admin rights). Autostart uses HKCU\...\CurrentVersion\Run.
#pragma once
#include <string>
#include <vector>

namespace bh {

struct Settings {
    int intervalSec = 60;       // periodic refresh
    int lowThreshold = 15;      // percent at or below which icons turn red
    bool hideDisconnected = false;
    bool trayIcons = false;     // false = inline on the taskbar (default), true = notification-area icons
    bool widgetLeft = true;     // taskbar widget at the left end (default) or next to the clock
    bool gameMode = true;       // hide + stop polling while a full-screen game/app is in front
    bool gameModeBrowsers = false;  // also when a browser (video) is full screen
    int menuTheme = 0;          // 0 = follow Windows, 1 = dark, 2 = light
    bool autoUpdate = true;     // check GitHub for a new version on start + daily (manual check always works)
    std::vector<std::wstring> order;  // device ids in the order the user dragged them

    void Load();
    void Save() const;
};

std::wstring GetAlias(const std::wstring& deviceId);
void SetAlias(const std::wstring& deviceId, const std::wstring& alias);  // empty = remove

// Last live reading per device (HKCU\Software\BatteryHub\LastKnown), so a device that
// is asleep when the app starts is still shown, dimmed, with its last percentage.
struct BatteryReading;
void SaveLastKnown(const BatteryReading& r);
std::vector<BatteryReading> LoadLastKnown();

bool IsAutostartEnabled();
bool SetAutostart(bool enable);

// Taskbar theme: SystemUsesLightTheme (taskbar/tray), AppsUseLightTheme as fallback.
bool IsTaskbarLight();

}  // namespace bh
