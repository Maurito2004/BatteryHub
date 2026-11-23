// Self-update from GitHub Releases (WinHTTP, no dependencies).
// Check: GET api.github.com/repos/<repo>/releases/latest -> tag_name + BatteryHub.exe asset.
// Install: download next to the running exe, rename the running exe to .old
// (Windows allows renaming a running exe), move the new one into place and
// start it with --after-update; it waits for this instance to exit.
#pragma once
#include <string>

namespace bh {

constexpr const wchar_t* kRepo = L"Maurito2004/BatteryHub";
constexpr const wchar_t* kReleasesPage = L"https://github.com/Maurito2004/BatteryHub/releases/latest";

struct UpdateInfo {
    bool ok = false;          // the check itself worked
    bool newer = false;       // latest release is newer than this build
    std::wstring latest;      // e.g. "1.2.0"
    std::wstring exeUrl;      // direct download of BatteryHub.exe
    std::wstring error;
};

std::wstring CurrentVersion();          // BH_VERSION (or BATTERYHUB_FAKE_VERSION env var, for testing)
int CompareVersions(const std::wstring& a, const std::wstring& b);  // <0, 0, >0
UpdateInfo CheckForUpdate();            // blocking: run off the UI thread
// Downloads and swaps the exe. On success the caller starts the new exe
// (StartUpdatedApp) and exits.
bool InstallUpdate(const UpdateInfo& info, std::wstring* error);
bool StartUpdatedApp();
void DeleteOldBinary();                 // leftovers from a previous update

}  // namespace bh
