// 2.4 GHz peripherals built on Compx / Nordic dongles (ATK, VXE, VGN mice and
// AULA keyboards). Request/reply layouts come from community reverse
// engineering (credits in README). All replies are printed by --dump.
#include "hid_models.h"

namespace bh {

// ---------------------------------------------------------------------------
// ATK / VXE / VGN mice, Compx dongle, vendor collection 0xFF02 with 17-byte reports.
//   request : 08 04 00 .. 00 CK      CK = 0x55 - sum(bytes 0..15)
//   reply   : 08 04 xx xx xx xx PCT CHG VH VL ...  (CHG 1 = charging, V = mV big-endian)
// No reply, or voltage 0, = mouse off / asleep.
// Sources: renanleonel/windows-taskbar-battery PR #5 (tested on 373B:10C9), Fan4Metal/ATK_tray.
bool ReadCompxMouse(HidDevice& dev, const HidModel& m, std::vector<BatteryReading>& out) {
    if (dev.info().outLen != 17) return false;  // the dongle also has a 20-byte 0xFF02 collection
    uint8_t req[17] = {0x08, 0x04};
    uint8_t sum = 0;
    for (int i = 0; i < 16; ++i) sum = (uint8_t)(sum + req[i]);
    req[16] = (uint8_t)(0x55 - sum);

    uint8_t r[64] = {};
    auto match = [](const uint8_t* p, int n) { return n >= 10 && p[0] == 0x08 && p[1] == 0x04; };
    int n = HidTransact(dev, req, sizeof(req), r, sizeof(r), match, 300);
    if (n < 0) return false;

    BatteryReading b = BaseReading(dev, m);
    b.id = Fmt(L"compx:%04x:%04x", dev.info().vid, dev.info().pid);
    int mv = (r[8] << 8) | r[9];
    if (n == 0 || mv == 0 || r[6] > 100) {
        b.online = false;  // asleep / switched off: keep the last value
    } else {
        b.percent = r[6];
        b.charge = r[7] == 1 ? Charge::Charging : Charge::Discharging;
        DLOG(L"      ATK/VXE: %d%%, %d mV, charging=%u", b.percent, mv, r[7]);
    }
    out.push_back(b);
    return n > 0;
}

// ---------------------------------------------------------------------------
// ATK 8K receivers with a Nordic nRF54 MCU (e.g. A9 Pro Max 373B:11DC), vendor
// collection 0xFF05/0x0001, 64-byte reports.
//   request : 08 7D 72 02 00 01 07 01 00..
//   reply   : in[1]==0x72 && in[5]==0x07; in[2]!=0 = asleep; in[7] = %, in[8..9] = mV (LE)
// Source: Lkig/ATK-A9-Battery README, Fan4Metal/ATK_tray. UNVERIFIED here.
bool ReadNordic54Mouse(HidDevice& dev, const HidModel& m, std::vector<BatteryReading>& out) {
    if (dev.info().outLen < 8) return false;
    const uint8_t req[] = {0x08, 0x7D, 0x72, 0x02, 0x00, 0x01, 0x07, 0x01};
    uint8_t r[80] = {};
    auto match = [](const uint8_t* p, int n) { return n >= 10 && p[1] == 0x72 && p[5] == 0x07; };
    int n = HidTransact(dev, req, sizeof(req), r, sizeof(r), match, 400);
    if (n < 0) return false;

    BatteryReading b = BaseReading(dev, m);
    b.id = Fmt(L"nordic:%04x:%04x", dev.info().vid, dev.info().pid);
    int mv = r[8] | (r[9] << 8);
    if (n == 0 || r[2] != 0 || r[7] > 100 || (r[7] == 0 && mv < 2000)) {
        b.online = false;
    } else {
        b.percent = r[7];
        b.charge = Charge::Discharging;
    }
    out.push_back(b);
    return n > 0;
}

// ---------------------------------------------------------------------------
// AULA keyboards (F75 / F75 Pro / AU75 / F87 / F99 / F68), Compx 2.4 GHz dongle
// VID 0x3554, vendor collection 0xFF02 with 20-byte reports.
//   request : 13 4A 00 .. 00 CK       CK = sum(bytes 0..18) mod 256 (= 0x5D)
//   reply   : 13 (4A|0x80) xx xx xx LVL STATE .. CK
//             STATE 0x01 = on battery, 0x10 = cable connected (level unreliable, ~100)
// The receiver also emits unrelated 0x13 frames: match the command byte.
// No reply = keyboard asleep or off. Each query resets the keyboard's sleep
// timer, so keep the refresh interval >= 45 s.
// Sources: Makkksx/AulaMonitor, DillanCidral/Joule PR #58.
bool ReadAulaKeyboard(HidDevice& dev, const HidModel& m, std::vector<BatteryReading>& out) {
    if (dev.info().outLen != 20) return false;
    uint8_t req[20] = {0x13, 0x4A};
    uint8_t sum = 0;
    for (int i = 0; i < 19; ++i) sum = (uint8_t)(sum + req[i]);
    req[19] = sum;

    uint8_t r[64] = {};
    auto match = [](const uint8_t* p, int n) { return n >= 7 && p[0] == 0x13 && (p[1] & 0x7F) == 0x4A; };
    int n = HidTransact(dev, req, sizeof(req), r, sizeof(r), match, 600);
    if (n < 0) return false;

    BatteryReading b = BaseReading(dev, m);
    b.id = Fmt(L"aula:%04x", dev.info().pid);
    if (n == 0) {
        b.online = false;
    } else {
        if (n >= 20) {
            uint8_t ck = 0;
            for (int i = 0; i < 19; ++i) ck = (uint8_t)(ck + r[i]);
            if (ck != r[19]) DLOG(L"      AULA: checksum mismatch (got %02X, expected %02X)", r[19], ck);
        }
        uint8_t level = r[5], state = r[6];
        if (state == 0x10) {
            b.charge = Charge::Charging;  // cable: keep the last real level (reported level is ~100)
            b.percent = -1;
        } else if (level >= 1 && level <= 100) {
            b.percent = level;
            b.charge = Charge::Discharging;
        } else {
            b.online = false;
        }
        DLOG(L"      AULA: level=%u state=0x%02X", level, state);
    }
    out.push_back(b);
    return n > 0;
}

}  // namespace bh
