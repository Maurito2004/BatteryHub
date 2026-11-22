// Razer mice: 90-byte feature report (report id 0), as documented by OpenRazer.
//   [status, transaction_id, remaining(2), protocol, data_size, class, id, args[80], crc, 0]
//   crc = XOR of bytes 2..87. Status 0x02 = OK, 0x01 = busy, 0x04 = timeout
//   (wireless mouse asleep), 0x05 = not supported.
//   Battery: class 0x07 id 0x80 -> args[1] 0..255; charging: class 0x07 id 0x84 -> args[1].
// The transaction id differs per model generation (0x1F newer, 0x3F older, 0xFF some);
// the working one is cached per PID.
#include <map>

#include "../core/log.h"
#include "hid_models.h"

namespace bh {
namespace {

std::map<uint16_t, uint8_t> g_tid;  // pid -> working transaction id

enum class RzStatus { Ok, Busy, Asleep, Fail };

RzStatus Command(HidDevice& d, uint8_t tid, uint8_t cls, uint8_t id, uint8_t size, uint8_t* args) {
    uint8_t buf[91] = {};
    uint8_t* r = buf + 1;
    r[1] = tid;
    r[5] = size;
    r[6] = cls;
    r[7] = id;
    uint8_t crc = 0;
    for (int k = 2; k < 88; ++k) crc ^= r[k];
    r[88] = crc;
    if (!d.SetFeature(buf, sizeof(buf))) return RzStatus::Fail;
    for (int attempt = 0; attempt < 8; ++attempt) {
        Sleep(attempt == 0 ? 30 : 25);
        uint8_t in[91] = {};
        if (!d.GetFeature(in, sizeof(in))) return RzStatus::Fail;
        uint8_t status = in[1];
        if (status == 0x01) continue;  // busy, ask again
        if (status == 0x04) return RzStatus::Asleep;
        if (status != 0x02 || in[1 + 6] != cls || in[1 + 7] != id) return RzStatus::Fail;
        memcpy(args, in + 1 + 8, 80);
        return RzStatus::Ok;
    }
    return RzStatus::Busy;
}

}  // namespace

bool ReadRazer(HidDevice& dev, const HidModel& m, std::vector<BatteryReading>& out) {
    if (dev.info().featLen < 91) return false;  // not the control collection
    BatteryReading r = BaseReading(dev, m);
    r.id = Fmt(L"razer:%04x", dev.info().pid);

    std::vector<uint8_t> tids = {0x1F, 0x3F, 0xFF};
    auto it = g_tid.find(dev.info().pid);
    if (it != g_tid.end()) tids.insert(tids.begin(), it->second);

    uint8_t args[80];
    for (uint8_t tid : tids) {
        RzStatus s = Command(dev, tid, 0x07, 0x80, 0x02, args);
        DLOG(L"      razer tid=0x%02X battery cmd -> %d", tid, (int)s);
        if (s == RzStatus::Asleep) {
            r.online = false;
            out.push_back(r);
            return true;
        }
        if (s != RzStatus::Ok) continue;
        g_tid[dev.info().pid] = tid;
        r.percent = (args[1] * 100 + 127) / 255;
        if (Command(dev, tid, 0x07, 0x84, 0x02, args) == RzStatus::Ok)
            r.charge = args[1] ? Charge::Charging : Charge::Discharging;
        if (r.charge == Charge::Charging && r.percent >= 100) r.charge = Charge::Full;
        out.push_back(r);
        return true;
    }
    return false;
}

}  // namespace bh
