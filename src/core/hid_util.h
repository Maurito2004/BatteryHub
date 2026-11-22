// Thin HID helpers over SetupAPI + hid.dll (no hidapi dependency).
#pragma once
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "device.h"

namespace bh {

struct HidInfo {
    std::wstring path;
    uint16_t vid = 0, pid = 0, version = 0;
    uint16_t usagePage = 0, usage = 0;
    uint16_t inLen = 0, outLen = 0, featLen = 0;  // report sizes incl. report-id byte
    int iface = -1;                               // USB interface number (&mi_XX), -1 if none
    std::wstring product, manufacturer, serial;
};

// All present HID top-level collections. Opening with zero access works even
// for keyboards/mice that Windows keeps exclusively.
std::vector<HidInfo> HidEnumerate();

// Identifies the physical device behind a collection path ("7&28d3d608" in
// "\\?\hid#vid_..&col04#7&28d3d608&0&0003#{...}"): shared by all collections of
// one device, different for two identical devices.
std::wstring HidInstanceToken(const std::wstring& path);

// Bluetooth / BLE / USB, deduced from the device path.
Connection ConnFromHidPath(const std::wstring& path);

class HidDevice {
public:
    explicit HidDevice(const HidInfo& info);
    ~HidDevice();
    HidDevice(const HidDevice&) = delete;
    HidDevice& operator=(const HidDevice&) = delete;

    bool ok() const { return h_ != INVALID_HANDLE_VALUE; }

    // Output report; data[0] is the report id. Padded to the device's output length.
    bool Write(const uint8_t* data, size_t len, DWORD timeoutMs = 500);
    // Input report; returns bytes read, 0 on timeout, -1 on error.
    int Read(uint8_t* buf, size_t len, DWORD timeoutMs);
    bool SetFeature(const uint8_t* data, size_t len);  // padded to featLen
    bool GetFeature(uint8_t* buf, size_t len);         // buf[0] = report id on input
    void FlushInput();

    const HidInfo& info() const { return info_; }

private:
    HidInfo info_;
    HANDLE h_ = INVALID_HANDLE_VALUE;
    HANDLE ev_ = nullptr;
};

std::wstring HexBytes(const uint8_t* p, size_t n);

// Milliseconds left until `deadline` (GetTickCount64 time), at least 1. Never
// underflows when the deadline has just passed (which would mean a ~49-day wait).
inline DWORD MsLeft(ULONGLONG deadline) {
    ULONGLONG now = GetTickCount64();
    return now >= deadline ? 1 : (DWORD)(deadline - now);
}

}  // namespace bh
