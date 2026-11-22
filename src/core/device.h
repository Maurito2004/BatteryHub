// BatteryHub - core device model shared by providers and UI.
#pragma once
#include <string>

namespace bh {

enum class DeviceKind { Unknown, Headset, Mouse, Keyboard, Gamepad };
enum class Connection { Unknown, Bluetooth, Wireless24, Wireless, Cable };
enum class Charge { Unknown, Discharging, Charging, Full };

// One battery reading produced by a provider during a poll.
struct BatteryReading {
    std::wstring id;      // stable key, e.g. "bt:{container}", "logi:c539:1"
    std::wstring name;    // product name as reported by the device/OS
    std::wstring source;  // provider that produced it (for --dump / menu)
    DeviceKind kind = DeviceKind::Unknown;
    Connection conn = Connection::Unknown;
    Charge charge = Charge::Unknown;
    int percent = -1;     // 0..100, -1 = unknown
    bool online = true;   // false = known device, but asleep/disconnected
};

const wchar_t* KindName(DeviceKind k);
const wchar_t* ConnName(Connection c);
const wchar_t* ChargeName(Charge c);

// Heuristic used when the transport does not tell us the device type.
DeviceKind GuessKindFromName(const std::wstring& name);

// Small printf-style helper returning std::wstring. Use %ls for wide strings.
std::wstring Fmt(const wchar_t* fmt, ...);

}  // namespace bh
