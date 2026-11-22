// Table-driven HID support. To add a device: add ONE line to kHidModels in
// hid_models.cpp pointing at an existing protocol reader (or write a new
// reader with the HidReadFn signature). Use `BatteryHub.exe --dump` to get the
// VID, PID, UsagePage, Usage and interface number of the collection to match.
#pragma once
#include <cstdint>
#include <vector>

#include "../core/device.h"
#include "../core/hid_util.h"
#include "../core/log.h"

namespace bh {

struct HidModel;

// A reader talks to one opened HID collection and appends 0..n readings
// (a Logitech receiver can return several paired devices). Returns false when
// the device did not answer the protocol at all (logged in --dump).
using HidReadFn = bool (*)(HidDevice& dev, const HidModel& model, std::vector<BatteryReading>& out);

struct HidModel {
    uint16_t vid;
    uint16_t pid;        // 0 = any product of this vendor
    uint16_t usagePage;  // 0 = any
    uint16_t usage;      // 0 = any
    int iface;           // USB interface (&mi_XX), -1 = any
    DeviceKind kind;     // Unknown = let the reader decide
    Connection conn;     // Unknown = deduce from the device path
    const wchar_t* name; // nullptr = HID product string
    HidReadFn read;
    bool verified;       // confirmed on real hardware; unverified readers are best effort
};

extern const HidModel kHidModels[];
extern const size_t kHidModelCount;

// Protocol readers (one file per vendor family).
bool ReadLogitechHidpp(HidDevice&, const HidModel&, std::vector<BatteryReading>&);
bool ReadRazer(HidDevice&, const HidModel&, std::vector<BatteryReading>&);
bool ReadArctis7(HidDevice&, const HidModel&, std::vector<BatteryReading>&);
bool ReadArctisNova7(HidDevice&, const HidModel&, std::vector<BatteryReading>&);
bool ReadArctisNovaPro(HidDevice&, const HidModel&, std::vector<BatteryReading>&);
bool ReadCorsairVoid(HidDevice&, const HidModel&, std::vector<BatteryReading>&);
bool ReadHyperXCloudAlpha(HidDevice&, const HidModel&, std::vector<BatteryReading>&);
bool ReadHyperXCloudFlight(HidDevice&, const HidModel&, std::vector<BatteryReading>&);
bool ReadPlayStation(HidDevice&, const HidModel&, std::vector<BatteryReading>&);

bool ReadCompxMouse(HidDevice&, const HidModel&, std::vector<BatteryReading>&);     // ATK / VXE / VGN
bool ReadNordic54Mouse(HidDevice&, const HidModel&, std::vector<BatteryReading>&);  // ATK 8K Nordic receivers
bool ReadAulaKeyboard(HidDevice&, const HidModel&, std::vector<BatteryReading>&);   // AULA F75 / F87 / F99...
bool ReadGameSir(HidDevice&, const HidModel&, std::vector<BatteryReading>&);        // GameSir 2.4 GHz pads

// Sends `req` as an output report (req[0] = report id, padded to the output
// length) and waits for an input report accepted by `match`. Every reply is
// logged in --dump. Returns bytes of the matching reply, 0 = none, -1 = write failed.
template <typename Match>
int HidTransact(HidDevice& d, const uint8_t* req, size_t reqLen, uint8_t* resp, size_t respLen, Match match,
                DWORD timeoutMs = 400, bool flush = true) {
    if (flush) d.FlushInput();
    if (!d.Write(req, reqLen)) {
        if (::bh::g_verbose) ::bh::Out(L"      write failed (%lu)", GetLastError());
        return -1;
    }
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline) {
        int n = d.Read(resp, respLen, MsLeft(deadline));
        if (n <= 0) break;
        if (::bh::g_verbose) ::bh::Out(L"      resp: %ls", HexBytes(resp, n < 40 ? n : 40).c_str());
        if (match(resp, n)) return n;
    }
    return 0;
}

// Fills id/name/kind/conn defaults from the model + HID info.
BatteryReading BaseReading(const HidDevice& dev, const HidModel& m, const wchar_t* idSuffix = nullptr);

// Li-ion voltage (mV) -> percent, piecewise linear (typical 1S cell curve).
int LiIonPercent(int millivolts);

}  // namespace bh
