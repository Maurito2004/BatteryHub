// =====================================================================
//  HID device table — add new VID/PID entries here.
//  Get the values from `BatteryHub.exe --dump` (section "HID").
//  Columns: VID, PID(0=any), UsagePage(0=any), Usage(0=any), Interface(-1=any),
//           kind, connection, display name (nullptr = HID product string),
//           reader, verified-on-hardware
// =====================================================================
#include "hid_models.h"

namespace bh {

using K = DeviceKind;
using C = Connection;

const HidModel kHidModels[] = {
    // ---- Logitech HID++ 2.0 (Unifying / Lightspeed / Bolt receivers and direct devices) ----
    // The long-report vendor collection (0xFF00/0x0002) is the HID++ channel.
    {0x046D, 0, 0xFF00, 0x0002, -1, K::Unknown, C::Unknown, nullptr, ReadLogitechHidpp, false},
    // Logitech G headsets (G533/G733/G935/PRO X Wireless) expose HID++ on 0xFF43/0x0202.
    {0x046D, 0, 0xFF43, 0x0202, -1, K::Headset, C::Wireless24, nullptr, ReadLogitechHidpp, false},

    // ---- Razer (90-byte feature report protocol, class 0x07 power) ----
    {0x1532, 0x007A, 0, 0, -1, K::Mouse, C::Cable,      L"Razer Viper Ultimate", ReadRazer, false},
    {0x1532, 0x007B, 0, 0, -1, K::Mouse, C::Wireless24, L"Razer Viper Ultimate", ReadRazer, false},
    {0x1532, 0x007C, 0, 0, -1, K::Mouse, C::Cable,      L"Razer DeathAdder V2 Pro", ReadRazer, false},
    {0x1532, 0x007D, 0, 0, -1, K::Mouse, C::Wireless24, L"Razer DeathAdder V2 Pro", ReadRazer, false},
    {0x1532, 0x0086, 0, 0, -1, K::Mouse, C::Cable,      L"Razer Basilisk Ultimate", ReadRazer, false},
    {0x1532, 0x0088, 0, 0, -1, K::Mouse, C::Wireless24, L"Razer Basilisk Ultimate", ReadRazer, false},
    {0x1532, 0x008F, 0, 0, -1, K::Mouse, C::Cable,      L"Razer Naga Pro", ReadRazer, false},
    {0x1532, 0x0090, 0, 0, -1, K::Mouse, C::Wireless24, L"Razer Naga Pro", ReadRazer, false},
    {0x1532, 0x00A5, 0, 0, -1, K::Mouse, C::Cable,      L"Razer Viper V2 Pro", ReadRazer, false},
    {0x1532, 0x00A6, 0, 0, -1, K::Mouse, C::Wireless24, L"Razer Viper V2 Pro", ReadRazer, false},
    {0x1532, 0x00AA, 0, 0, -1, K::Mouse, C::Cable,      L"Razer Basilisk V3 Pro", ReadRazer, false},
    {0x1532, 0x00AB, 0, 0, -1, K::Mouse, C::Wireless24, L"Razer Basilisk V3 Pro", ReadRazer, false},
    {0x1532, 0x00B6, 0, 0, -1, K::Mouse, C::Cable,      L"Razer DeathAdder V3 Pro", ReadRazer, false},
    {0x1532, 0x00B7, 0, 0, -1, K::Mouse, C::Wireless24, L"Razer DeathAdder V3 Pro", ReadRazer, false},

    // ---- SteelSeries Arctis ----
    {0x1038, 0x1260, 0, 0, 5, K::Headset, C::Wireless24, L"SteelSeries Arctis 7", ReadArctis7, false},
    {0x1038, 0x12AD, 0, 0, 5, K::Headset, C::Wireless24, L"SteelSeries Arctis 7 (2019)", ReadArctis7, false},
    {0x1038, 0x1252, 0, 0, 5, K::Headset, C::Wireless24, L"SteelSeries Arctis Pro Wireless", ReadArctis7, false},
    {0x1038, 0x2202, 0, 0, 3, K::Headset, C::Wireless24, L"SteelSeries Arctis Nova 7", ReadArctisNova7, false},
    {0x1038, 0x2206, 0, 0, 3, K::Headset, C::Wireless24, L"SteelSeries Arctis Nova 7X", ReadArctisNova7, false},
    {0x1038, 0x220A, 0, 0, 3, K::Headset, C::Wireless24, L"SteelSeries Arctis Nova 7P", ReadArctisNova7, false},
    {0x1038, 0x12E0, 0, 0, 4, K::Headset, C::Wireless24, L"SteelSeries Arctis Nova Pro Wireless", ReadArctisNovaPro, false},
    {0x1038, 0x12E5, 0, 0, 4, K::Headset, C::Wireless24, L"SteelSeries Arctis Nova Pro Wireless X", ReadArctisNovaPro, false},

    // ---- Corsair (legacy wireless headset protocol, vendor page 0xFFC5) ----
    {0x1B1C, 0x0A14, 0xFFC5, 0, -1, K::Headset, C::Wireless24, L"Corsair VOID PRO Wireless", ReadCorsairVoid, false},
    {0x1B1C, 0x0A16, 0xFFC5, 0, -1, K::Headset, C::Wireless24, L"Corsair VOID PRO Wireless", ReadCorsairVoid, false},
    {0x1B1C, 0x0A1A, 0xFFC5, 0, -1, K::Headset, C::Wireless24, L"Corsair VOID PRO Wireless", ReadCorsairVoid, false},
    {0x1B1C, 0x0A38, 0xFFC5, 0, -1, K::Headset, C::Wireless24, L"Corsair HS70 Wireless", ReadCorsairVoid, false},
    {0x1B1C, 0x0A4F, 0xFFC5, 0, -1, K::Headset, C::Wireless24, L"Corsair HS70 PRO Wireless", ReadCorsairVoid, false},
    {0x1B1C, 0x0A55, 0xFFC5, 0, -1, K::Headset, C::Wireless24, L"Corsair VOID RGB ELITE Wireless", ReadCorsairVoid, false},

    // ---- HyperX (Kingston 0x0951 / HP 0x03F0) ----
    {0x03F0, 0x098D, 0, 0, -1, K::Headset, C::Wireless24, L"HyperX Cloud Alpha Wireless", ReadHyperXCloudAlpha, false},
    {0x0951, 0x16C4, 0, 0, -1, K::Headset, C::Wireless24, L"HyperX Cloud Flight", ReadHyperXCloudFlight, false},
    {0x0951, 0x1723, 0, 0, -1, K::Headset, C::Wireless24, L"HyperX Cloud Flight", ReadHyperXCloudFlight, false},

    // ---- ATK / VXE / VGN mice (Compx dongles: 17-byte reports on 0xFF02) ----
    // Reader checks the 17-byte collection itself, so any PID of these vendors is tried.
    {0x373B, 0, 0xFF02, 0x0002, -1, K::Mouse, C::Wireless24, L"ATK / VXE", ReadCompxMouse, true},
    {0x3554, 0xF503, 0xFF02, 0x0002, -1, K::Mouse, C::Wireless24, L"VGN F1 Pro", ReadCompxMouse, false},
    {0x3554, 0xF58A, 0xFF02, 0x0002, -1, K::Mouse, C::Wireless24, L"VXE R1 Pro Max", ReadCompxMouse, false},
    {0x3554, 0xF58E, 0xFF02, 0x0002, -1, K::Mouse, C::Wireless24, L"VXE R1 SE+", ReadCompxMouse, false},
    // ATK 8K receivers with a Nordic nRF54 MCU (A9 Pro Max, Zero...)
    {0x373B, 0, 0xFF05, 0x0001, -1, K::Mouse, C::Wireless24, L"ATK 8K", ReadNordic54Mouse, false},

    // ---- AULA keyboards (Compx dongle VID 0x3554, 20-byte reports on 0xFF02) ----
    {0x3554, 0xFA09, 0xFF02, 0x0002, -1, K::Keyboard, C::Wireless24, L"AULA F75", ReadAulaKeyboard, true},
    {0x3554, 0xFA08, 0xFF02, 0x0002, -1, K::Keyboard, C::Wireless24, L"AULA F68", ReadAulaKeyboard, false},
    {0x3554, 0xFA0A, 0xFF02, 0x0002, -1, K::Keyboard, C::Wireless24, L"AULA F87", ReadAulaKeyboard, false},
    {0x3554, 0xFA10, 0xFF02, 0x0002, -1, K::Keyboard, C::Wireless24, L"AULA F87", ReadAulaKeyboard, false},
    {0x3554, 0xFA11, 0xFF02, 0x0002, -1, K::Keyboard, C::Wireless24, L"AULA F99", ReadAulaKeyboard, false},

    // ---- GameSir 2.4 GHz controllers (vendor collections used by GameSir Connect) ----
    {0x3537, 0, 0xFFF0, 0x0040, -1, K::Gamepad, C::Wireless24, nullptr, ReadGameSir, false},
    {0x3537, 0, 0xFF7A, 0, -1, K::Gamepad, C::Wireless24, nullptr, ReadGameSir, false},

    // ---- Sony PlayStation (input report status byte; Generic Desktop / Game Pad) ----
    {0x054C, 0x05C4, 0x0001, 0x0005, -1, K::Gamepad, C::Unknown, L"DualShock 4", ReadPlayStation, false},
    {0x054C, 0x09CC, 0x0001, 0x0005, -1, K::Gamepad, C::Unknown, L"DualShock 4", ReadPlayStation, false},
    {0x054C, 0x0BA0, 0x0001, 0x0005, -1, K::Gamepad, C::Wireless24, L"DualShock 4 (USB adapter)", ReadPlayStation, false},
    {0x054C, 0x0CE6, 0x0001, 0x0005, -1, K::Gamepad, C::Unknown, L"DualSense", ReadPlayStation, false},
    {0x054C, 0x0DF2, 0x0001, 0x0005, -1, K::Gamepad, C::Unknown, L"DualSense Edge", ReadPlayStation, false},
};

const size_t kHidModelCount = sizeof(kHidModels) / sizeof(kHidModels[0]);

BatteryReading BaseReading(const HidDevice& dev, const HidModel& m, const wchar_t* idSuffix) {
    const HidInfo& i = dev.info();
    BatteryReading r;
    r.id = Fmt(L"hid:%04x:%04x", i.vid, i.pid);
    if (!i.serial.empty()) r.id += L":" + i.serial;
    if (idSuffix) r.id += idSuffix;
    r.name = m.name ? m.name : i.product;
    r.kind = m.kind != DeviceKind::Unknown ? m.kind : GuessKindFromName(r.name);
    r.conn = m.conn != Connection::Unknown ? m.conn : ConnFromHidPath(i.path);
    return r;
}

int LiIonPercent(int mv) {
    static const int curve[][2] = {{4186, 100}, {4067, 90}, {3989, 80}, {3922, 70}, {3859, 60},
                                   {3811, 50},  {3778, 40}, {3751, 30}, {3717, 20}, {3671, 10},
                                   {3646, 5},   {3579, 2},  {3500, 0}};
    if (mv >= curve[0][0]) return 100;
    for (size_t k = 1; k < sizeof(curve) / sizeof(curve[0]); ++k) {
        if (mv >= curve[k][0]) {
            int v0 = curve[k][0], p0 = curve[k][1], v1 = curve[k - 1][0], p1 = curve[k - 1][1];
            return p0 + (mv - v0) * (p1 - p0) / (v1 - v0);
        }
    }
    return 0;
}

}  // namespace bh
