// GameSir controllers through their 2.4 GHz dongle (VID 0x3537). In XInput
// mode the pad reports no battery to Windows, so it is read from the vendor
// HID collection the GameSir Connect app uses. UNVERIFIED on this machine:
// run --dump with the pad on and check the "resp:" lines.
//
//  * Cyclone 2 / G7 Pro 8K family — collection 0xFFF0/0x0040:
//      request 0F 0B (read profile; the G7 Pro 8K answers with the state report)
//      or heartbeat 0F F2 00 (Cyclone 2 streams report 0x12 while it is sent)
//      reply   report 0x12: [35] bit0 = charging, [36] = battery % (report id = byte 0)
//      an all-zero 0x12 frame = pad asleep
//    Sources: HeyOkay/HaloBattery PR #390 (tested on 3537:10C6), pedrosegato/gamesir-reverse-engineering.
//  * Nova 2 Lite — collection 0xFF7A:
//      request 00 01 01 (65-byte output report)
//      reply   [1] == 0x01, [19] = battery % or 0xFF = charging; skip frames F4 00
//    Source: AJAY0406/Gamesir-Nova-2-Lite-Battery-Detection.
#include "hid_models.h"

namespace bh {
namespace {

const wchar_t* GameSirName(uint16_t pid, const std::wstring& product) {
    switch (pid) {
        case 0x100B: case 0x102A: case 0x1053: case 0x101D: return L"GameSir Cyclone 2";
        case 0x108E: case 0x109F: return L"GameSir Cyclone 3";
        case 0x10B7: case 0x10B9: case 0x10CB: case 0x10CC: case 0x10CD: case 0x10CE:
        case 0x10C5: case 0x10C6: case 0x10C7: case 0x10C8: case 0x10C9: case 0x10CA:
        case 0x10FD: case 0x10FE: case 0x1032: case 0x1033: case 0x1034: case 0x1035:
            return L"GameSir G7 Pro";
        case 0x1098: case 0x100F: return L"GameSir Nova 2 Lite";
        case 0x10A9: return L"GameSir G7 Pro Xbox";
    }
    return product.empty() ? L"GameSir" : nullptr;
}

bool AllZero(const uint8_t* p, int n) {
    for (int i = 1; i < n; ++i)
        if (p[i]) return false;
    return true;
}

}  // namespace

bool ReadGameSir(HidDevice& dev, const HidModel& m, std::vector<BatteryReading>& out) {
    const HidInfo& info = dev.info();
    if (info.outLen == 0) return false;
    BatteryReading b = BaseReading(dev, m);
    // Per-dongle id: two identical pads/dongles must not merge into one entry.
    b.id = Fmt(L"gamesir:%04x:%ls", info.pid, HidInstanceToken(info.path).c_str());
    if (const wchar_t* nm = GameSirName(info.pid, info.product)) b.name = nm;
    b.kind = DeviceKind::Gamepad;
    // USB-cable identities of the pads (the dongles have their own PIDs).
    static const uint16_t kWiredPids[] = {0x1053, 0x101D, 0x10B7, 0x10CB, 0x10CD, 0x10C5,
                                          0x10C7, 0x10C9, 0x10FD, 0x1032, 0x1034};
    b.conn = Connection::Wireless24;
    for (uint16_t p : kWiredPids)
        if (info.pid == p) b.conn = Connection::Cable;

    uint8_t r[128] = {};
    if (info.usagePage == 0xFF7A) {  // Nova 2 Lite
        const uint8_t req[] = {0x00, 0x01, 0x01};
        auto match = [](const uint8_t* p, int n) {
            return n >= 20 && p[1] == 0x01 && !(p[1] == 0xF4 && p[2] == 0x00);
        };
        int n = HidTransact(dev, req, sizeof(req), r, sizeof(r), match, 1200);
        if (n <= 0) {
            out.push_back(b);  // connected, battery unknown: still listed ("--")
            return false;
        }
        uint8_t v = r[19];
        if (v == 0xFF) {
            b.charge = Charge::Charging;
            b.percent = -1;  // keep the last wireless reading
        } else if (v <= 100) {
            b.percent = v;
            b.charge = Charge::Discharging;
        } else {
            b.online = false;
        }
        out.push_back(b);
        return true;
    }

    // 0xFFF0 / 0x0040 family (Cyclone 2, G7 Pro 8K, Cyclone 3)
    auto isState = [](const uint8_t* p, int n) { return n >= 37 && p[0] == 0x12; };
    const uint8_t profileReq[] = {0x0F, 0x0B};
    int n = HidTransact(dev, profileReq, sizeof(profileReq), r, sizeof(r), isState, 500);
    if (n == 0) {
        const uint8_t heartbeat[] = {0x0F, 0xF2, 0x00};
        n = HidTransact(dev, heartbeat, sizeof(heartbeat), r, sizeof(r), isState, 700);
    }
    if (n <= 0) {
        // Silent pad, e.g. G7 Pro Xbox / "Wuchang" edition (3537:10A9): GameSir Nexus reads its
        // battery through an undocumented channel and Windows.Gaming.Input only returns a
        // 1000/1000 mWh placeholder. List it as connected with unknown battery.
        out.push_back(b);
        return false;
    }
    if (AllZero(r, n) || r[36] > 100) {
        b.online = false;  // pad asleep / off: dongle streams empty frames
    } else {
        b.percent = r[36];
        b.charge = (r[35] & 0x01) ? Charge::Charging : Charge::Discharging;
    }
    DLOG(L"      GameSir: [35]=0x%02X [36]=%u", r[35], r[36]);
    out.push_back(b);
    return true;
}

}  // namespace bh
