// Sony DualShock 4 / DualSense, read straight from the HID input report
// (layouts as in the Linux hid-playstation driver):
//   DS4  USB report 0x01: status byte [30]; BT report 0x11: [32]
//        low nibble = level 0..10 (11 = full when cabled), bit 4 = cable
//   DS5  USB report 0x01: status byte [53]; BT report 0x31: [54]
//        low nibble = level 0..10, high nibble 0=discharging 1=charging 2=full
// Over Bluetooth both pads send a reduced report until a calibration feature
// report is read (0x02 for DS4, 0x05 for DualSense), which this reader does.
#include "../core/log.h"
#include "hid_models.h"

namespace bh {

bool ReadPlayStation(HidDevice& dev, const HidModel& m, std::vector<BatteryReading>& out) {
    const HidInfo& info = dev.info();
    const bool dualsense = info.pid == 0x0CE6 || info.pid == 0x0DF2;
    const bool bt = ConnFromHidPath(info.path) == Connection::Bluetooth;

    if (bt) {
        uint8_t feat[80] = {(uint8_t)(dualsense ? 0x05 : 0x02)};
        dev.GetFeature(feat, dualsense ? 41 : 37);  // switches the pad to full reports
    }
    const uint8_t wantId = bt ? (dualsense ? 0x31 : 0x11) : 0x01;
    const size_t off = dualsense ? (bt ? 54 : 53) : (bt ? 32 : 30);

    std::vector<uint8_t> buf(info.inLen > 64 ? info.inLen : 64);
    dev.FlushInput();
    for (int tries = 0; tries < 8; ++tries) {
        int n = dev.Read(buf.data(), buf.size(), 150);
        if (n <= 0) break;
        if (buf[0] != wantId || (size_t)n <= off) {
            DLOG(L"      report 0x%02X (%d bytes) skipped", buf[0], n);
            continue;
        }
        uint8_t s = buf[off];
        DLOG(L"      status byte [%zu] = 0x%02X", off, s);
        BatteryReading r = BaseReading(dev, m, bt ? L":bt" : L":usb");
        if (r.conn == Connection::Unknown) r.conn = bt ? Connection::Bluetooth : Connection::Cable;
        int level = s & 0x0F;
        if (dualsense) {
            int state = s >> 4;
            r.percent = level * 10 + 5 > 100 ? 100 : level * 10 + 5;
            r.charge = state == 1 ? Charge::Charging : state == 2 ? Charge::Full : Charge::Discharging;
            if (state == 2) r.percent = 100;
        } else {
            bool cable = (s & 0x10) != 0;
            if (cable) {
                r.percent = level >= 10 ? 100 : level * 10 + 5;
                r.charge = level >= 10 ? Charge::Full : Charge::Charging;
            } else {
                r.percent = level * 10 + 5 > 100 ? 100 : level * 10 + 5;
                r.charge = Charge::Discharging;
            }
        }
        out.push_back(r);
        return true;
    }
    return false;
}

}  // namespace bh
