#include "settings.h"

#include <windows.h>

#include <cwchar>

#include "device.h"

namespace bh {

static const wchar_t kKey[] = L"Software\\BatteryHub";
static const wchar_t kAliasKey[] = L"Software\\BatteryHub\\Aliases";
static const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t kRunValue[] = L"BatteryHub";

static DWORD ReadDword(HKEY root, const wchar_t* sub, const wchar_t* name, DWORD def) {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(root, sub, name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS) return v;
    return def;
}

static void WriteDword(const wchar_t* name, DWORD v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kKey, name, REG_DWORD, &v, sizeof(v));
}

static int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void Settings::Load() {
    intervalSec = Clamp((int)ReadDword(HKEY_CURRENT_USER, kKey, L"IntervalSec", 60), 10, 3600);
    lowThreshold = Clamp((int)ReadDword(HKEY_CURRENT_USER, kKey, L"LowThreshold", 15), 1, 90);
    hideDisconnected = ReadDword(HKEY_CURRENT_USER, kKey, L"HideDisconnected", 0) != 0;
    trayIcons = ReadDword(HKEY_CURRENT_USER, kKey, L"TrayIcons", 0) != 0;
    widgetLeft = ReadDword(HKEY_CURRENT_USER, kKey, L"WidgetLeft", 1) != 0;
}

void Settings::Save() const {
    WriteDword(L"IntervalSec", (DWORD)intervalSec);
    WriteDword(L"LowThreshold", (DWORD)lowThreshold);
    WriteDword(L"HideDisconnected", hideDisconnected ? 1 : 0);
    WriteDword(L"TrayIcons", trayIcons ? 1 : 0);
    WriteDword(L"WidgetLeft", widgetLeft ? 1 : 0);
}

std::wstring GetAlias(const std::wstring& id) {
    wchar_t buf[256];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kAliasKey, id.c_str(), RRF_RT_REG_SZ, nullptr, buf, &size) ==
        ERROR_SUCCESS)
        return buf;
    return {};
}

void SetAlias(const std::wstring& id, const std::wstring& alias) {
    if (alias.empty()) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kAliasKey, id.c_str());
        return;
    }
    RegSetKeyValueW(HKEY_CURRENT_USER, kAliasKey, id.c_str(), REG_SZ, alias.c_str(),
                    (DWORD)((alias.size() + 1) * sizeof(wchar_t)));
}

static const wchar_t kLastKey[] = L"Software\\BatteryHub\\LastKnown";

void SaveLastKnown(const BatteryReading& r) {
    // "percent|kind|conn|name"
    std::wstring v = Fmt(L"%d|%d|%d|%ls", r.percent, (int)r.kind, (int)r.conn, r.name.c_str());
    RegSetKeyValueW(HKEY_CURRENT_USER, kLastKey, r.id.c_str(), REG_SZ, v.c_str(),
                    (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}

std::vector<BatteryReading> LoadLastKnown() {
    std::vector<BatteryReading> list;
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kLastKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return list;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256], data[300];
        DWORD nameLen = 256, dataSize = sizeof(data), type = 0;
        if (RegEnumValueW(key, i, name, &nameLen, nullptr, &type, (BYTE*)data, &dataSize) != ERROR_SUCCESS) break;
        if (type != REG_SZ) continue;
        int pct = -1, kind = 0, conn = 0, consumed = 0;
        if (swscanf(data, L"%d|%d|%d|%n", &pct, &kind, &conn, &consumed) < 3 || pct < 0 || pct > 100) continue;
        BatteryReading r;
        r.id = name;
        r.percent = pct;
        r.kind = (DeviceKind)kind;
        r.conn = (Connection)conn;
        r.name = data + consumed;
        r.online = false;
        list.push_back(r);
    }
    RegCloseKey(key);
    return list;
}

static std::wstring RunCommand() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::wstring(L"\"") + path + L"\"";
}

bool IsAutostartEnabled() {
    wchar_t buf[MAX_PATH + 8];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, buf, &size) !=
        ERROR_SUCCESS)
        return false;
    // Enabled only if it points at *this* exe (the portable exe may have moved).
    return _wcsicmp(buf, RunCommand().c_str()) == 0;
}

bool SetAutostart(bool enable) {
    if (!enable) {
        LSTATUS s = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
        return s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND;
    }
    std::wstring cmd = RunCommand();
    return RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, cmd.c_str(),
                           (DWORD)((cmd.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool IsTaskbarLight() {
    const wchar_t* k = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
    DWORD sys = ReadDword(HKEY_CURRENT_USER, k, L"SystemUsesLightTheme", 0xFFFFFFFF);
    if (sys != 0xFFFFFFFF) return sys != 0;
    return ReadDword(HKEY_CURRENT_USER, k, L"AppsUseLightTheme", 0) != 0;  // pre-1903 fallback
}

}  // namespace bh
