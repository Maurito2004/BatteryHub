// Xbox controllers (Xbox Wireless Adapter / USB / Bluetooth via xinputhid)
// through XInputGetBatteryInformation. xinput1_4.dll is loaded at runtime so
// the exe has no hard dependency and still starts if it is missing.
#include <windows.h>
#include <xinput.h>

#include "../core/log.h"
#include "../core/provider.h"

namespace bh {
namespace {

using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using GetBatteryFn = DWORD(WINAPI*)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION*);

class XInputProvider : public Provider {
public:
    XInputProvider() {
        for (const wchar_t* dll : {L"xinput1_4.dll", L"xinput9_1_0.dll"}) {
            lib_ = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (lib_) break;
        }
        if (lib_) {
            getState_ = (GetStateFn)(void*)GetProcAddress(lib_, "XInputGetState");
            getBattery_ = (GetBatteryFn)(void*)GetProcAddress(lib_, "XInputGetBatteryInformation");
        }
    }
    ~XInputProvider() override {
        if (lib_) FreeLibrary(lib_);
    }

    const wchar_t* Name() const override { return L"XInput (Xbox controllers)"; }

    void Poll(std::vector<BatteryReading>& out) override {
        if (!getState_ || !getBattery_) {
            DLOG(L"  xinput1_4.dll not available (XInputGetBatteryInformation missing)");
            return;
        }
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
            XINPUT_STATE st;
            DWORD rc = getState_(i, &st);
            if (rc != ERROR_SUCCESS) {
                DLOG(L"  slot %lu: not connected", i);
                continue;
            }
            XINPUT_BATTERY_INFORMATION bi = {};
            rc = getBattery_(i, BATTERY_DEVTYPE_GAMEPAD, &bi);
            DLOG(L"  slot %lu: connected, rc=%lu  BatteryType=%u  BatteryLevel=%u", i, rc, bi.BatteryType,
                 bi.BatteryLevel);
            // DISCONNECTED/UNKNOWN: e.g. Bluetooth pads (the BT provider reports them).
            // WIRED: XInput gives no level at all, and third-party 2.4 GHz pads (GameSir...)
            // always claim "wired" — those are read by their vendor HID reader instead.
            if (rc != ERROR_SUCCESS || bi.BatteryType == BATTERY_TYPE_DISCONNECTED ||
                bi.BatteryType == BATTERY_TYPE_UNKNOWN || bi.BatteryType == BATTERY_TYPE_WIRED)
                continue;

            BatteryReading r;
            r.id = Fmt(L"xinput:%lu", i);
            r.name = Fmt(L"Xbox Controller %lu", i + 1);
            r.kind = DeviceKind::Gamepad;
            r.conn = Connection::Wireless;
            r.charge = Charge::Discharging;
            // XInput only exposes 4 coarse levels.
            static const int map[] = {5, 30, 65, 100};
            r.percent = map[bi.BatteryLevel & 3];
            out.push_back(r);
        }
    }

private:
    HMODULE lib_ = nullptr;
    GetStateFn getState_ = nullptr;
    GetBatteryFn getBattery_ = nullptr;
};

}  // namespace

std::unique_ptr<Provider> CreateXInputProvider() { return std::make_unique<XInputProvider>(); }

}  // namespace bh
