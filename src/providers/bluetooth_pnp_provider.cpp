// Bluetooth (classic + LE) battery via the PnP property the Windows Bluetooth
// stack publishes ({104EA319-...},2 — the value shown in Settings > Bluetooth).
// Covers most headsets (HFP battery report), BLE mice/keyboards and earbuds,
// without opening the device. One pass over all present devnodes, grouped by
// ContainerId so the multiple nodes of one physical device collapse into one.
#include <windows.h>
#include <cfgmgr32.h>

#include <cwctype>
#include <map>

#include "../core/log.h"
#include "../core/provider.h"
#include "bt_common.h"

#ifdef _MSC_VER
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#endif

namespace bh {
namespace {

struct Agg {
    int battery = -1;
    std::wstring name, btNodeName, batteryNode;
    unsigned cod = 0;
    int connected = -1;  // -1 unknown, from IsConnected property
    bool started = false, isBt = false;
};

bool StartsWithI(const std::wstring& s, const wchar_t* prefix) {
    return _wcsnicmp(s.c_str(), prefix, wcslen(prefix)) == 0;
}

DeviceKind KindFromCod(unsigned cod) {
    unsigned major = (cod >> 8) & 0x1F, minor = (cod >> 2) & 0x3F;
    if (major == 0x04) return DeviceKind::Headset;  // Audio/Video
    if (major == 0x05) {                            // Peripheral
        if ((minor & 0x0F) == 1 || (minor & 0x0F) == 2) return DeviceKind::Gamepad;
        unsigned kb = (minor >> 4) & 0x3;
        if (kb == 1) return DeviceKind::Keyboard;
        if (kb == 2 || kb == 3) return DeviceKind::Mouse;
    }
    return DeviceKind::Unknown;
}

class BluetoothPnpProvider : public Provider {
public:
    const wchar_t* Name() const override { return L"Bluetooth (PnP battery property)"; }

    void Poll(std::vector<BatteryReading>& out) override {
        HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
        if (set == INVALID_HANDLE_VALUE) {
            DLOG(L"  SetupDiGetClassDevs failed (%lu)", GetLastError());
            return;
        }
        std::map<std::wstring, Agg> byContainer;
        SP_DEVINFO_DATA dev = {sizeof(dev)};
        for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
            wchar_t inst[512];
            if (!SetupDiGetDeviceInstanceIdW(set, &dev, inst, 512, nullptr)) continue;
            std::wstring instance = inst;
            bool btNode = StartsWithI(instance, L"BTHENUM\\") || StartsWithI(instance, L"BTHLE\\") ||
                          StartsWithI(instance, L"BTHLEDEVICE\\");
            unsigned battery = 0;
            bool hasBattery = GetPropU32(set, &dev, kPkeyBluetoothBattery, &battery);
            if (!btNode && !hasBattery) continue;

            GUID cg;
            if (!GetPropGuid(set, &dev, kPkeyContainerId, &cg)) continue;
            std::wstring container = GuidToString(cg);
            if (IsLocalMachineContainer(container)) continue;

            Agg& a = byContainer[container];
            a.isBt |= btNode;
            std::wstring fname = GetPropString(set, &dev, kPkeyFriendlyName);
            if (fname.empty()) fname = GetPropString(set, &dev, kPkeyDeviceDesc);
            // The BTHENUM\DEV_xxx / BTHLE\DEV_xxx node carries the user-visible name.
            if ((StartsWithI(instance, L"BTHENUM\\DEV_") || StartsWithI(instance, L"BTHLE\\DEV_")) &&
                !fname.empty())
                a.btNodeName = fname;
            if (a.name.empty()) a.name = fname;
            unsigned v;
            if (GetPropU32(set, &dev, kPkeyBtClassOfDevice, &v) && v) a.cod = v;
            if (GetPropU32(set, &dev, kPkeyIsConnected, &v)) a.connected = (a.connected == 1 || v) ? 1 : 0;
            ULONG status = 0, problem = 0;
            if (CM_Get_DevNode_Status(&status, &problem, dev.DevInst, 0) == CR_SUCCESS &&
                (status & DN_STARTED) && problem == 0)
                a.started = true;
            if (hasBattery && (int)battery > a.battery) {
                a.battery = (int)battery;
                a.batteryNode = instance;
            }
        }
        SetupDiDestroyDeviceInfoList(set);

        for (auto& [container, a] : byContainer) {
            if (!a.isBt) continue;
            std::wstring name = a.btNodeName.empty() ? a.name : a.btNodeName;
            // When the stack exposes IsConnected trust it; else fall back to "node started".
            bool online = a.connected >= 0 ? a.connected == 1 : a.started;
            DLOG(L"  %-34ls battery=%3d%%  connected=%ls  started=%ls  CoD=0x%06X  container=%ls",
                 name.c_str(), a.battery, a.connected < 0 ? L"?" : (a.connected ? L"yes" : L"no"),
                 a.started ? L"yes" : L"no", a.cod, container.c_str());
            if (a.battery < 0) continue;
            DLOG(L"      battery node: %ls", a.batteryNode.c_str());
            BatteryReading r;
            r.id = L"bt:" + container;
            r.name = name;
            r.kind = KindFromCod(a.cod);
            if (r.kind == DeviceKind::Unknown) r.kind = GuessKindFromName(name);
            r.conn = Connection::Bluetooth;
            r.percent = a.battery > 100 ? 100 : a.battery;
            r.online = online;
            out.push_back(r);
        }
    }
};

}  // namespace

std::unique_ptr<Provider> CreateBluetoothPnpProvider() { return std::make_unique<BluetoothPnpProvider>(); }

}  // namespace bh
