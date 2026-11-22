// Wireless headset dongles: SteelSeries Arctis, Corsair VOID/HS, HyperX Cloud.
// Request/response byte layouts follow the community-documented protocols
// (as used by HeadsetControl). They are UNVERIFIED here: confirm with
// `BatteryHub.exe --dump`, which prints every raw response.
#include "../core/log.h"
#include "hid_models.h"

namespace bh {
namespace {

template <typename Match>
int Transact(HidDevice& d, const uint8_t* req, size_t reqLen, uint8_t* resp, size_t respLen, Match match,
             DWORD timeoutMs = 400) {
    return HidTransact(d, req, reqLen, resp, respLen, match, timeoutMs);
}

const auto kAny = [](const uint8_t*, int) { return true; };

}  // namespace

// SteelSeries Arctis 7 / 7 2019 / Pro Wireless: out {0x06,0x18} -> in[2] = percent.
bool ReadArctis7(HidDevice& dev, const HidModel& m, std::vector<BatteryReading>& out) {
    const uint8_t req[] = {0x06, 0x18};
    uint8_t r[64];
    if (Transact(dev, req, sizeof(req), r, sizeof(r), kAny) < 3) return false;
    BatteryReading b = BaseReading(dev, m);
    b.percent = r[2] > 100 ? 100 : r[2];
    out.push_back(b);
    return true;
}

// SteelSeries Arctis Nova 7: out {0x00,0xB0} -> in[2] level 0..4, in[3] 0=offline 1=charging 3=online.
bool ReadArctisNova7(HidDevice& dev, const HidModel& m, std::vector<BatteryReading>& out) {
    const uint8_t req[] = {0x00, 0xB0};
    uint8_t r[64];
    if (Transact(dev, req, sizeof(req), r, sizeof(r), kAny) < 4) return false;
    BatteryReading b = BaseReading(dev, m);
    if (r[3] == 0x00) {
        b.online = false;
    } else {
        int lvl = r[2] > 4 ? 4 : r[2];
        b.percent = lvl * 25;
        b.charge = r[3] == 0x01 ? Charge::Charging : Charge::Discharging;
    }
    out.push_back(b);
    return true;
}

// SteelSeries Arctis Nova Pro Wireless: out {0x06,0xB0} -> in[6] level 0..8, in[15] 1=offline 2=charging 8=online.
bool ReadArctisNovaPro(HidDevice& dev, const HidModel& m, std::vector<BatteryReading>& out) {
    const uint8_t req[] = {0x06, 0xB0};
    uint8_t r[128];
    if (Transact(dev, req, sizeof(req), r, sizeof(r), kAny) < 16) return false;
    BatteryReading b = BaseReading(dev, m);
    if (r[15] == 0x01) {
        b.online = false;
    } else {
        int lvl = r[6] > 8 ? 8 : r[6];
        b.percent = lvl * 100 / 8;
        b.charge = r[15] == 0x02 ? Charge::Charging : Charge::Discharging;
    }
    out.push_back(b);
    return true;
}

// Corsair VOID / HS70 (legacy): out {0xC9,0x64} -> in report 0x64: [2]&0x7F percent,
// [4] 0=disconnected 1=ok 2=low 4/5=charging.
bool ReadCorsairVoid(HidDevice& dev, const HidModel& m, std::vector<BatteryReading>& out) {
    const uint8_t req[] = {0xC9, 0x64};
    uint8_t r[64];
    auto match = [](const uint8_t* p, int n) { return n >= 5 && p[0] == 0x64; };
    if (Transact(dev, req, sizeof(req), r, sizeof(r), match) < 5) return false;
    BatteryReading b = BaseReading(dev, m);
    if (r[4] == 0) {
        b.online = false;
    } else {
        b.percent = r[2] & 0x7F;
        if (b.percent > 100) b.percent = 100;
        b.charge = (r[4] == 4 || r[4] == 5) ? Charge::Charging : Charge::Discharging;
    }
    out.push_back(b);
    return true;
}

// HyperX Cloud Alpha Wireless: out {0x21,0xBB,0x0B} -> in {0x21,0xBB,0x0B,percent};
// charging: {0x21,0xBB,0x0C} -> in[3] == 1.
bool ReadHyperXCloudAlpha(HidDevice& dev, const HidModel& m, std::vector<BatteryReading>& out) {
    if (dev.info().usagePage < 0xFF00 || dev.info().outLen == 0) return false;  // vendor channel only
    uint8_t req[] = {0x21, 0xBB, 0x0B};
    uint8_t r[64];
    auto match0B = [](const uint8_t* p, int n) { return n >= 4 && p[0] == 0x21 && p[2] == 0x0B; };
    if (Transact(dev, req, sizeof(req), r, sizeof(r), match0B) < 4) return false;
    BatteryReading b = BaseReading(dev, m);
    b.percent = r[3] > 100 ? 100 : r[3];
    req[2] = 0x0C;
    auto match0C = [](const uint8_t* p, int n) { return n >= 4 && p[0] == 0x21 && p[2] == 0x0C; };
    if (Transact(dev, req, sizeof(req), r, sizeof(r), match0C) >= 4)
        b.charge = r[3] == 1 ? Charge::Charging : Charge::Discharging;
    out.push_back(b);
    return true;
}

// HyperX Cloud Flight: out {0x21,0xFF,0x05} -> in[0]==0x21? voltage = in[3]<<8 | in[4] (mV).
bool ReadHyperXCloudFlight(HidDevice& dev, const HidModel& m, std::vector<BatteryReading>& out) {
    if (dev.info().outLen == 0) return false;
    const uint8_t req[] = {0x21, 0xFF, 0x05};
    uint8_t r[64];
    auto match = [](const uint8_t* p, int n) { return n >= 5 && p[0] == 0x21; };
    if (Transact(dev, req, sizeof(req), r, sizeof(r), match) < 5) return false;
    int mv = (r[3] << 8) | r[4];
    BatteryReading b = BaseReading(dev, m);
    if (mv < 3000 || mv > 4400) {
        b.online = false;  // headset off: dongle answers without a plausible voltage
    } else {
        b.percent = LiIonPercent(mv);
    }
    out.push_back(b);
    return true;
}

}  // namespace bh
