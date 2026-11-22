// Enumerates HID collections once per poll and dispatches matching ones to the
// protocol readers declared in hid_models.cpp. In --dump mode it lists every
// collection (VID, PID, UsagePage, Usage, interface, report sizes) and flags
// those advertising standard HID battery usages, to help add new devices.
#include <windows.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}

#include <set>

#include "../core/log.h"
#include "../core/provider.h"
#include "hid_models.h"

namespace bh {
namespace {

bool Matches(const HidModel& m, const HidInfo& i) {
    return m.vid == i.vid && (m.pid == 0 || m.pid == i.pid) && (m.usagePage == 0 || m.usagePage == i.usagePage) &&
           (m.usage == 0 || m.usage == i.usage) && (m.iface < 0 || m.iface == i.iface);
}

// Standard battery usages: Generic Device Controls "Battery Strength" (0x06/0x20)
// and Battery System page 0x85 (AbsoluteStateOfCharge 0x65, RemainingCapacity 0x66).
std::wstring BatteryUsages(const HidInfo& info) {
    HANDLE h = CreateFileW(info.path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring found;
    PHIDP_PREPARSED_DATA pp = nullptr;
    if (HidD_GetPreparsedData(h, &pp)) {
        HIDP_CAPS caps;
        if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) {
            const HIDP_REPORT_TYPE types[] = {HidP_Input, HidP_Feature};
            const USHORT counts[] = {caps.NumberInputValueCaps, caps.NumberFeatureValueCaps};
            for (int t = 0; t < 2; ++t) {
                USHORT n = counts[t];
                if (!n) continue;
                std::vector<HIDP_VALUE_CAPS> vc(n);
                if (HidP_GetValueCaps(types[t], vc.data(), &n, pp) != HIDP_STATUS_SUCCESS) continue;
                for (USHORT k = 0; k < n; ++k) {
                    USHORT up = vc[k].UsagePage;
                    USHORT u = vc[k].IsRange ? vc[k].Range.UsageMin : vc[k].NotRange.Usage;
                    if ((up == 0x06 && u == 0x20) || (up == 0x85 && (u == 0x65 || u == 0x66)))
                        found += Fmt(L"%ls %04X:%02X (report %u) ", t ? L"feature" : L"input", up, u,
                                     vc[k].ReportID);
                }
            }
        }
        HidD_FreePreparsedData(pp);
    }
    CloseHandle(h);
    return found;
}

class HidProvider : public Provider {
public:
    const wchar_t* Name() const override { return L"HID (vendor protocols, PlayStation)"; }

    void Poll(std::vector<BatteryReading>& out) override {
        auto list = HidEnumerate();
        DLOG(L"  %zu HID collections present", list.size());
        std::set<std::wstring> satisfied;  // devices already read through another collection

        for (const auto& info : list) {
            const HidModel* model = nullptr;
            for (size_t k = 0; k < kHidModelCount; ++k)
                if (Matches(kHidModels[k], info)) {
                    model = &kHidModels[k];
                    break;
                }

            if (g_verbose) {
                Out(L"  VID=%04X PID=%04X UsagePage=%04X Usage=%04X MI=%-2d in/out/feat=%u/%u/%u  \"%ls\" [%ls]",
                    info.vid, info.pid, info.usagePage, info.usage, info.iface, info.inLen, info.outLen,
                    info.featLen, info.product.c_str(), info.manufacturer.c_str());
                std::wstring bu = BatteryUsages(info);
                if (!bu.empty()) Out(L"      advertises HID battery usage: %ls", bu.c_str());
                if (model) {
                    Out(L"      -> matched reader%ls", model->verified ? L"" : L" (unverified)");
                    Out(L"      path: %ls", info.path.c_str());
                }
            }
            if (!model) continue;

            std::wstring devKey = Fmt(L"%04x:%04x:%ls", info.vid, info.pid, info.serial.c_str());
            if (model->read != ReadLogitechHidpp && satisfied.count(devKey)) {
                DLOG(L"      already read via another collection");
                continue;
            }

            HidDevice dev(info);
            if (!dev.ok()) {
                DLOG(L"      open failed (error %lu) - device busy or access denied", GetLastError());
                continue;
            }
            std::vector<BatteryReading> part;
            bool ok = model->read(dev, *model, part);
            for (auto& r : part) {
                DLOG(L"      => %ls: %d%% %ls, %ls, %ls  id=%ls", r.name.c_str(), r.percent, ChargeName(r.charge),
                     ConnName(r.conn), r.online ? L"online" : L"offline/asleep", r.id.c_str());
                if (r.online && r.percent >= 0) satisfied.insert(devKey);
                out.push_back(std::move(r));
            }
            if (!ok) DLOG(L"      no valid answer from this collection");
        }
    }
};

}  // namespace

std::unique_ptr<Provider> CreateHidProvider() { return std::make_unique<HidProvider>(); }

// `--probe VID PID USAGEPAGE [hex bytes...]` (dev aid): opens the matching
// collection, optionally writes one output report, and prints every input
// report received for 2 s. With no bytes it only listens.
int RunHidProbe(uint16_t vid, uint16_t pid, uint16_t usagePage, const std::vector<uint8_t>& bytes) {
    int found = 0;
    for (const auto& info : HidEnumerate()) {
        if (info.vid != vid || info.pid != pid || info.usagePage != usagePage) continue;
        ++found;
        Out(L"collection %04X:%04X page %04X usage %04X in/out/feat=%u/%u/%u", info.vid, info.pid, info.usagePage,
            info.usage, info.inLen, info.outLen, info.featLen);
        HidDevice dev(info);
        if (!dev.ok()) {
            Out(L"  open failed (%lu)", GetLastError());
            continue;
        }
        dev.FlushInput();
        if (!bytes.empty()) {
            bool ok = dev.Write(bytes.data(), bytes.size());
            Out(L"  write %ls-> %ls", HexBytes(bytes.data(), bytes.size()).c_str(),
                ok ? L"ok" : Fmt(L"FAILED (%lu)", GetLastError()).c_str());
        }
        ULONGLONG end = GetTickCount64() + 2000;
        int reports = 0;
        uint8_t buf[512];
        while (GetTickCount64() < end) {
            int n = dev.Read(buf, sizeof(buf), MsLeft(end));
            if (n <= 0) break;
            Out(L"  in[%d]: %ls", n, HexBytes(buf, n < 64 ? n : 64).c_str());
            if (++reports >= 12) break;
        }
        if (!reports) Out(L"  (no input reports in 2 s)");
    }
    if (!found) Out(L"no collection %04X:%04X page %04X", vid, pid, usagePage);
    return 0;
}

}  // namespace bh
