// A provider knows one way of reading batteries (a transport or protocol).
// Poll() runs on the worker thread, never concurrently with itself. In --dump
// mode (bh::g_verbose) it also logs everything it sees with DLOG().
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include "device.h"

namespace bh {

class Provider {
public:
    virtual ~Provider() = default;
    virtual const wchar_t* Name() const = 0;
    virtual void Poll(std::vector<BatteryReading>& out) = 0;
};

std::unique_ptr<Provider> CreateBluetoothPnpProvider();  // DEVPKEY battery property
std::unique_ptr<Provider> CreateBleGattProvider();       // GATT Battery Service 0x180F
std::unique_ptr<Provider> CreateXInputProvider();        // Xbox controllers
std::unique_ptr<Provider> CreateHidProvider();           // vendor HID protocols (+ PlayStation)

// Dev aid for adding devices: see main.cpp `--probe`.
int RunHidProbe(uint16_t vid, uint16_t pid, uint16_t usagePage, const std::vector<uint8_t>& bytes);

}  // namespace bh
