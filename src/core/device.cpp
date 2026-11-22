#include "device.h"
#include "log.h"

#include <windows.h>
#include <cstdarg>
#include <cwchar>
#include <cwctype>

namespace bh {

const wchar_t* KindName(DeviceKind k) {
    switch (k) {
        case DeviceKind::Headset:  return L"Auriculares";
        case DeviceKind::Mouse:    return L"Mouse";
        case DeviceKind::Keyboard: return L"Teclado";
        case DeviceKind::Gamepad:  return L"Joystick";
        default:                   return L"Dispositivo";
    }
}

const wchar_t* ConnName(Connection c) {
    switch (c) {
        case Connection::Bluetooth:  return L"BT";
        case Connection::Wireless24: return L"2.4 GHz";
        case Connection::Wireless:   return L"Inalámbrico";
        case Connection::Cable:      return L"Cable";
        default:                     return L"?";
    }
}

const wchar_t* ChargeName(Charge c) {
    switch (c) {
        case Charge::Discharging: return L"descargando";
        case Charge::Charging:    return L"cargando";
        case Charge::Full:        return L"cargada";
        default:                  return L"-";
    }
}

static bool ContainsI(const std::wstring& hay, const wchar_t* needle) {
    std::wstring h = hay, n = needle;
    for (auto& ch : h) ch = (wchar_t)towlower(ch);
    for (auto& ch : n) ch = (wchar_t)towlower(ch);
    return h.find(n) != std::wstring::npos;
}

DeviceKind GuessKindFromName(const std::wstring& name) {
    static const wchar_t* headset[] = {L"headset", L"headphone", L"buds", L"airpods", L"arctis",
                                       L"cloud", L"wh-", L"wf-", L"earbud", L"auricular", L"hands-free",
                                       L"void", L"virtuoso", L"barracuda", L"jabra", L"jbl", L"audio"};
    static const wchar_t* mouse[] = {L"mouse", L"mx master", L"mx anywhere", L"viper", L"deathadder",
                                     L"basilisk", L"g pro", L"g502", L"g305", L"g703", L"g903", L"aerox",
                                     L"rival", L"mamba", L"naga", L"trackball", L"ratón", L"raton"};
    static const wchar_t* keyboard[] = {L"keyboard", L"keys", L"teclado", L"mx mechanical", L"k380", L"k780"};
    static const wchar_t* pad[] = {L"controller", L"gamepad", L"dualsense", L"dualshock", L"xbox",
                                   L"joy-con", L"pro controller", L"joystick", L"wireless controller"};
    for (auto s : pad) if (ContainsI(name, s)) return DeviceKind::Gamepad;
    for (auto s : keyboard) if (ContainsI(name, s)) return DeviceKind::Keyboard;
    for (auto s : mouse) if (ContainsI(name, s)) return DeviceKind::Mouse;
    for (auto s : headset) if (ContainsI(name, s)) return DeviceKind::Headset;
    return DeviceKind::Unknown;
}

std::wstring Fmt(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnwprintf(buf, 2047, fmt, ap);
    va_end(ap);
    if (n < 0) n = 2047;
    buf[n] = 0;
    return buf;
}

}  // namespace bh
