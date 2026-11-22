// BLE devices exposing the standard GATT Battery Service (0x180F, Battery
// Level characteristic 0x2A19). Fallback for LE devices whose battery is not
// published as a PnP property. Reads the stack's cached value (no radio
// traffic, never blocks on an asleep device); HID-over-GATT devices usually
// deny this access and are covered by the PnP provider instead.
#include <windows.h>
#include <setupapi.h>
extern "C" {
#include <bluetoothleapis.h>
}

#include <vector>

#include "../core/log.h"
#include "../core/provider.h"
#include "bt_common.h"

#ifdef _MSC_VER
#pragma comment(lib, "bluetoothapis.lib")
#pragma comment(lib, "setupapi.lib")
#endif

namespace bh {
namespace {

// GUID_BLUETOOTHLE_DEVICE_INTERFACE
const GUID kBleDeviceInterface = {0x781AEE18, 0x7733, 0x4CE4, {0xAD, 0xD0, 0x91, 0xF4, 0x1C, 0x67, 0xB5, 0x92}};

bool IsShort(const BTH_LE_UUID& u, USHORT v) { return u.IsShortUuid && u.Value.ShortUuid == v; }

int ReadBatteryLevel(HANDLE h, HRESULT* err) {
    USHORT n = 0;
    *err = BluetoothGATTGetServices(h, 0, nullptr, &n, BLUETOOTH_GATT_FLAG_NONE);
    if (n == 0) return -1;
    std::vector<BTH_LE_GATT_SERVICE> svcs(n);
    *err = BluetoothGATTGetServices(h, n, svcs.data(), &n, BLUETOOTH_GATT_FLAG_NONE);
    if (FAILED(*err)) return -1;
    for (auto& s : svcs) {
        if (!IsShort(s.ServiceUuid, 0x180F)) continue;
        USHORT cn = 0;
        BluetoothGATTGetCharacteristics(h, &s, 0, nullptr, &cn, BLUETOOTH_GATT_FLAG_NONE);
        if (cn == 0) return -1;
        std::vector<BTH_LE_GATT_CHARACTERISTIC> chars(cn);
        *err = BluetoothGATTGetCharacteristics(h, &s, cn, chars.data(), &cn, BLUETOOTH_GATT_FLAG_NONE);
        if (FAILED(*err)) return -1;
        for (auto& c : chars) {
            if (!IsShort(c.CharacteristicUuid, 0x2A19) || !c.IsReadable) continue;
            USHORT size = 0;
            BluetoothGATTGetCharacteristicValue(h, &c, 0, nullptr, &size, BLUETOOTH_GATT_FLAG_NONE);
            if (size == 0) return -1;
            std::vector<BYTE> buf(size);
            auto* val = reinterpret_cast<PBTH_LE_GATT_CHARACTERISTIC_VALUE>(buf.data());
            *err = BluetoothGATTGetCharacteristicValue(h, &c, size, val, nullptr, BLUETOOTH_GATT_FLAG_NONE);
            if (FAILED(*err) || val->DataSize < 1) return -1;
            return val->Data[0];
        }
    }
    *err = S_FALSE;  // no battery service
    return -1;
}

class BleGattProvider : public Provider {
public:
    const wchar_t* Name() const override { return L"Bluetooth LE (GATT Battery Service 0x180F)"; }

    void Poll(std::vector<BatteryReading>& out) override {
        HDEVINFO set = SetupDiGetClassDevsW(&kBleDeviceInterface, nullptr, nullptr,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set == INVALID_HANDLE_VALUE) return;
        SP_DEVICE_INTERFACE_DATA ifd = {sizeof(ifd)};
        for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &kBleDeviceInterface, i, &ifd); ++i) {
            DWORD need = 0;
            SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
            if (!need) continue;
            std::vector<BYTE> buf(need);
            auto* det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
            det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            SP_DEVINFO_DATA dev = {sizeof(dev)};
            if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, nullptr, &dev)) continue;

            std::wstring name = GetPropString(set, &dev, kPkeyFriendlyName);
            GUID cg = {};
            std::wstring container = GetPropGuid(set, &dev, kPkeyContainerId, &cg) ? GuidToString(cg) : L"";
            unsigned connected = 1;
            bool knowsConn = GetPropU32(set, &dev, kPkeyIsConnected, &connected);

            HANDLE h = CreateFileW(det->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                DLOG(L"  %-34ls open failed (%lu)", name.c_str(), GetLastError());
                continue;
            }
            HRESULT err = S_OK;
            int level = ReadBatteryLevel(h, &err);
            CloseHandle(h);
            DLOG(L"  %-34ls battery=%3d%%  connected=%ls  hr=0x%08lX  container=%ls", name.c_str(), level,
                 knowsConn ? (connected ? L"yes" : L"no") : L"?", (unsigned long)err, container.c_str());
            if (level < 0 || container.empty() || (knowsConn && !connected)) continue;  // paired, not connected

            BatteryReading r;
            r.id = L"bt:" + container;  // same key as the PnP provider -> merged
            r.name = name;
            r.kind = GuessKindFromName(name);
            r.conn = Connection::Bluetooth;
            r.percent = level > 100 ? 100 : level;
            r.online = !knowsConn || connected;
            out.push_back(r);
        }
        SetupDiDestroyDeviceInfoList(set);
    }
};

}  // namespace

std::unique_ptr<Provider> CreateBleGattProvider() { return std::make_unique<BleGattProvider>(); }

}  // namespace bh
