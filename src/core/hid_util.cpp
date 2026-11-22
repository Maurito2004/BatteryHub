#include "hid_util.h"

#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}
#include <cwctype>

#ifdef _MSC_VER
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#endif

namespace bh {

static std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

Connection ConnFromHidPath(const std::wstring& path) {
    std::wstring p = Lower(path);
    // HID over GATT (BLE) and classic Bluetooth HID service UUIDs / enumerators.
    if (p.find(L"{00001812-") != std::wstring::npos || p.find(L"bthledevice") != std::wstring::npos ||
        p.find(L"{00001124-") != std::wstring::npos || p.find(L"bthenum") != std::wstring::npos ||
        p.find(L"_vid&0002") != std::wstring::npos)  // "_vid&0002xxxx" = Bluetooth vendor-id source
        return Connection::Bluetooth;
    return Connection::Cable;  // USB; providers refine to Wireless24 for dongles
}

static int IfaceFromPath(const std::wstring& path) {
    std::wstring p = Lower(path);
    size_t i = p.find(L"&mi_");
    if (i == std::wstring::npos) return -1;
    return (int)wcstol(p.c_str() + i + 4, nullptr, 16);
}

std::vector<HidInfo> HidEnumerate() {
    std::vector<HidInfo> list;
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return list;

    SP_DEVICE_INTERFACE_DATA ifd = {sizeof(ifd)};
    for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, idx, &ifd); ++idx) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::vector<BYTE> buf(need);
        auto* det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, nullptr, nullptr)) continue;

        HidInfo info;
        info.path = det->DevicePath;
        info.iface = IfaceFromPath(info.path);

        HANDLE h = CreateFileW(info.path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attr = {sizeof(attr)};
        if (HidD_GetAttributes(h, &attr)) {
            info.vid = attr.VendorID;
            info.pid = attr.ProductID;
            info.version = attr.VersionNumber;
        }
        PHIDP_PREPARSED_DATA pp = nullptr;
        if (HidD_GetPreparsedData(h, &pp)) {
            HIDP_CAPS caps;
            if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) {
                info.usagePage = caps.UsagePage;
                info.usage = caps.Usage;
                info.inLen = caps.InputReportByteLength;
                info.outLen = caps.OutputReportByteLength;
                info.featLen = caps.FeatureReportByteLength;
            }
            HidD_FreePreparsedData(pp);
        }
        wchar_t s[128];
        if (HidD_GetProductString(h, s, sizeof(s))) info.product = s;
        if (HidD_GetManufacturerString(h, s, sizeof(s))) info.manufacturer = s;
        if (HidD_GetSerialNumberString(h, s, sizeof(s))) info.serial = s;
        CloseHandle(h);
        list.push_back(std::move(info));
    }
    SetupDiDestroyDeviceInfoList(set);
    return list;
}

HidDevice::HidDevice(const HidInfo& info) : info_(info) {
    h_ = CreateFileW(info.path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                     nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h_ == INVALID_HANDLE_VALUE) {
        // Some vendor collections are write-only or read-only; try read-only for feature reports.
        h_ = CreateFileW(info.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    }
    ev_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

HidDevice::~HidDevice() {
    if (h_ != INVALID_HANDLE_VALUE) {
        CancelIo(h_);
        CloseHandle(h_);
    }
    if (ev_) CloseHandle(ev_);
}

static bool WaitIo(HANDLE h, HANDLE ev, OVERLAPPED* ov, DWORD timeoutMs, DWORD* transferred) {
    if (WaitForSingleObject(ev, timeoutMs) != WAIT_OBJECT_0) {
        CancelIo(h);
        GetOverlappedResult(h, ov, transferred, TRUE);  // let the cancel complete
        return false;
    }
    return GetOverlappedResult(h, ov, transferred, FALSE) != 0;
}

bool HidDevice::Write(const uint8_t* data, size_t len, DWORD timeoutMs) {
    if (!ok() || info_.outLen == 0) return false;
    std::vector<uint8_t> buf(info_.outLen > len ? info_.outLen : len, 0);
    memcpy(buf.data(), data, len);
    OVERLAPPED ov = {};
    ov.hEvent = ev_;
    ResetEvent(ev_);
    DWORD done = 0;
    if (!WriteFile(h_, buf.data(), (DWORD)info_.outLen, &done, &ov)) {
        if (GetLastError() != ERROR_IO_PENDING) return false;
        return WaitIo(h_, ev_, &ov, timeoutMs, &done);
    }
    return true;
}

int HidDevice::Read(uint8_t* out, size_t len, DWORD timeoutMs) {
    if (!ok() || info_.inLen == 0) return -1;
    std::vector<uint8_t> buf(info_.inLen, 0);
    OVERLAPPED ov = {};
    ov.hEvent = ev_;
    ResetEvent(ev_);
    DWORD done = 0;
    if (!ReadFile(h_, buf.data(), (DWORD)buf.size(), &done, &ov)) {
        if (GetLastError() != ERROR_IO_PENDING) return -1;
        if (!WaitIo(h_, ev_, &ov, timeoutMs, &done)) return 0;
    }
    size_t n = done < len ? done : len;
    memcpy(out, buf.data(), n);
    return (int)n;
}

bool HidDevice::SetFeature(const uint8_t* data, size_t len) {
    if (!ok()) return false;
    std::vector<uint8_t> buf(info_.featLen > len ? info_.featLen : len, 0);
    memcpy(buf.data(), data, len);
    return HidD_SetFeature(h_, buf.data(), (ULONG)buf.size()) != 0;
}

bool HidDevice::GetFeature(uint8_t* out, size_t len) {
    if (!ok()) return false;
    std::vector<uint8_t> buf(info_.featLen > len ? info_.featLen : len, 0);
    buf[0] = out[0];
    if (!HidD_GetFeature(h_, buf.data(), (ULONG)buf.size())) return false;
    memcpy(out, buf.data(), len);
    return true;
}

void HidDevice::FlushInput() {
    if (ok()) HidD_FlushQueue(h_);
}

std::wstring HexBytes(const uint8_t* p, size_t n) {
    std::wstring s;
    wchar_t b[4];
    for (size_t i = 0; i < n; ++i) {
        _snwprintf(b, 4, L"%02X ", p[i]);
        s += b;
    }
    return s;
}

}  // namespace bh
