// Logitech HID++ 2.0 over the long-report vendor collection.
//   request: [0x11, deviceIndex, featureIndex, (function<<4)|swId, params...] (20 bytes)
//   error  : [0x11, deviceIndex, 0xFF, featureIndex, fn|swId, errorCode]
// Receivers (Unifying/Lightspeed/Bolt) host up to 6 devices at index 1..6;
// directly attached devices (cable, own dongle, Bluetooth) answer at 0xFF.
// Battery features tried in order: 0x1004 Unified Battery, 0x1000 Battery
// Status, 0x1001 Battery Voltage, 0x1F20 ADC Measurement (headsets).
#include <map>

#include "../core/log.h"
#include "hid_models.h"

namespace bh {
namespace {

constexpr uint8_t kSwId = 0x0A;
constexpr DWORD kTimeoutMs = 300;

struct LogiCache {
    uint8_t battIndex = 0;
    uint16_t battFeature = 0;
    std::wstring name;
    DeviceKind kind = DeviceKind::Unknown;
};
std::map<std::wstring, LogiCache> g_cache;  // worker thread only

class Hidpp {
public:
    Hidpp(HidDevice& d, uint8_t index) : d_(d), idx_(index) {}

    // Returns true and fills resp[16] on success; *err gets the HID++ error code.
    bool Call(uint8_t feat, uint8_t fn, const uint8_t* params, size_t np, uint8_t* resp, int* err = nullptr) {
        uint8_t req[20] = {0x11, idx_, feat, (uint8_t)((fn << 4) | kSwId)};
        for (size_t k = 0; k < np && k < 16; ++k) req[4 + k] = params[k];
        if (err) *err = -1;
        if (!d_.Write(req, sizeof(req))) return false;
        ULONGLONG deadline = GetTickCount64() + kTimeoutMs;
        uint8_t r[64];
        while (GetTickCount64() < deadline) {
            int n = d_.Read(r, sizeof(r), MsLeft(deadline));
            if (n <= 0) break;
            if (n < 7 || (r[0] != 0x11 && r[0] != 0x10) || r[1] != idx_) continue;
            if ((r[2] == 0xFF || r[2] == 0x8F) && r[3] == feat) {  // HID++ 2.0 / 1.0 error
                if (err) *err = r[5];
                return false;
            }
            if (r[2] == feat && r[3] == req[3]) {
                memcpy(resp, r + 4, 16);
                if (err) *err = 0;
                return true;
            }
        }
        return false;
    }

    uint8_t FeatureIndex(uint16_t feature) {
        uint8_t p[2] = {(uint8_t)(feature >> 8), (uint8_t)feature}, r[16];
        return Call(0x00, 0x0, p, 2, r) ? r[0] : 0;
    }

    bool Ping() {
        uint8_t p[3] = {0, 0, 0x5A}, r[16];
        return Call(0x00, 0x1, p, 3, r);
    }

private:
    HidDevice& d_;
    uint8_t idx_;
};

bool IsReceiver(const HidInfo& i) {
    static const uint16_t pids[] = {0xC52B, 0xC52F, 0xC531, 0xC532, 0xC534, 0xC539, 0xC53A, 0xC53D,
                                    0xC53F, 0xC541, 0xC545, 0xC547, 0xC548, 0xC54D};
    for (auto p : pids)
        if (i.pid == p) return true;
    return i.product.find(L"Receiver") != std::wstring::npos;
}

DeviceKind KindFromHidppType(uint8_t t) {
    switch (t) {
        case 0: case 2: return DeviceKind::Keyboard;
        case 3: case 4: case 5: return DeviceKind::Mouse;
        case 8: return DeviceKind::Headset;
        case 11: case 12: return DeviceKind::Gamepad;
        default: return DeviceKind::Unknown;
    }
}

void Describe(Hidpp& h, LogiCache& c) {
    uint8_t idx = h.FeatureIndex(0x0005), r[16];  // Device Name & Type
    if (idx) {
        if (h.Call(idx, 0x0, nullptr, 0, r)) {
            int len = r[0];
            std::string name;
            while ((int)name.size() < len) {
                uint8_t p = (uint8_t)name.size();
                if (!h.Call(idx, 0x1, &p, 1, r)) break;
                size_t before = name.size();
                for (int k = 0; k < 16 && (int)name.size() < len && r[k]; ++k) name += (char)r[k];
                if (name.size() == before) break;
            }
            c.name.assign(name.begin(), name.end());
        }
        if (h.Call(idx, 0x2, nullptr, 0, r)) c.kind = KindFromHidppType(r[0]);
    }
    for (uint16_t f : {0x1004, 0x1000, 0x1001, 0x1F20}) {
        uint8_t fi = h.FeatureIndex(f);
        if (fi) {
            c.battIndex = fi;
            c.battFeature = f;
            break;
        }
    }
}

bool ReadBattery(Hidpp& h, const LogiCache& c, BatteryReading& r) {
    uint8_t p[16] = {};
    switch (c.battFeature) {
        case 0x1004:  // Unified Battery: get_status
            if (!h.Call(c.battIndex, 0x1, nullptr, 0, p)) return false;
            r.percent = p[0];
            r.charge = p[2] == 0 ? Charge::Discharging : p[2] == 3 ? Charge::Full
                     : (p[2] == 1 || p[2] == 2) ? Charge::Charging : Charge::Unknown;
            if (r.percent == 0 && p[1]) {  // devices reporting only coarse levels
                r.percent = (p[1] & 8) ? 100 : (p[1] & 4) ? 60 : (p[1] & 2) ? 20 : 5;
            }
            return true;
        case 0x1000:  // Battery Status: GetBatteryLevelStatus
            if (!h.Call(c.battIndex, 0x0, nullptr, 0, p)) return false;
            r.percent = p[0];
            r.charge = p[2] == 0 ? Charge::Discharging : p[2] == 3 ? Charge::Full
                     : (p[2] == 1 || p[2] == 2 || p[2] == 4) ? Charge::Charging : Charge::Unknown;
            return true;
        case 0x1001:  // Battery Voltage
        case 0x1F20:  // ADC Measurement
        {
            if (!h.Call(c.battIndex, 0x0, nullptr, 0, p)) return false;
            int mv = (p[0] << 8) | p[1];
            if (mv == 0) return false;
            r.percent = LiIonPercent(mv);
            if (c.battFeature == 0x1001)
                r.charge = (p[2] & 0x80) ? Charge::Charging : Charge::Discharging;
            else
                r.charge = (p[2] & 0x02) ? Charge::Charging : Charge::Discharging;
            DLOG(L"      voltage %d mV, flags 0x%02X", mv, p[2]);
            return true;
        }
    }
    return false;
}

}  // namespace

bool ReadLogitechHidpp(HidDevice& dev, const HidModel& m, std::vector<BatteryReading>& out) {
    const HidInfo& info = dev.info();
    const bool receiver = IsReceiver(info);
    bool any = false;
    dev.FlushInput();

    std::vector<uint8_t> indices;
    if (receiver) indices = {1, 2, 3, 4, 5, 6};
    else indices = {0xFF};

    for (uint8_t index : indices) {
        BatteryReading r = BaseReading(dev, m, Fmt(L":%u", index).c_str());
        r.id = Fmt(L"logi:%04x:%u", info.pid, index);
        if (receiver) r.conn = Connection::Wireless24;
        auto cached = g_cache.find(r.id);

        Hidpp h(dev, index);
        if (!h.Ping()) {
            // Empty slot, or paired device asleep: report offline only if we knew it.
            if (cached != g_cache.end()) {
                r.name = cached->second.name;
                r.kind = cached->second.kind;
                r.online = false;
                out.push_back(r);
            }
            DLOG(L"      HID++ index %u: no answer", index);
            continue;
        }
        any = true;
        if (cached == g_cache.end() || !cached->second.battIndex) {
            LogiCache c;
            Describe(h, c);
            cached = g_cache.insert_or_assign(r.id, c).first;
        }
        const LogiCache& c = cached->second;
        if (!c.name.empty()) r.name = c.name;
        if (c.kind != DeviceKind::Unknown) r.kind = c.kind;
        if (r.kind == DeviceKind::Unknown) r.kind = GuessKindFromName(r.name);
        DLOG(L"      HID++ index %u: \"%ls\" battery feature 0x%04X at index %u", index, c.name.c_str(),
             c.battFeature, c.battIndex);
        if (!c.battIndex) continue;
        if (!ReadBattery(h, c, r)) {
            g_cache.erase(r.id);  // feature table may have changed (other device paired)
            r.online = false;
        }
        out.push_back(r);
    }
    return any;
}

}  // namespace bh
