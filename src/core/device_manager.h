// Aggregates providers and keeps the last known state of every device, so a
// device that goes to sleep keeps its last percentage (shown dimmed).
#pragma once
#include <memory>
#include <vector>

#include "provider.h"

namespace bh {

struct TrackedDevice {
    BatteryReading r;        // last good data; r.online reflects the latest poll
    bool stale = false;      // true when the percentage is not from the latest poll
    unsigned long long lastSeen = 0;  // GetTickCount64() of the last live reading
};

class DeviceManager {
public:
    DeviceManager();

    // Worker thread: query every provider and merge duplicates by id.
    std::vector<BatteryReading> PollAll();

    // UI thread: fold a poll result into the tracked state.
    void Apply(std::vector<BatteryReading> fresh);

    const std::vector<TrackedDevice>& Devices() const { return devices_; }

    // Tray app only: start with last-known values (dimmed) and persist new live ones.
    void EnablePersistence();

private:
    std::vector<std::unique_ptr<Provider>> providers_;
    std::vector<TrackedDevice> devices_;  // in order of first appearance
    bool persist_ = false;
};

// Merge rule used for duplicates within one poll (e.g. PnP + GATT for one
// Bluetooth device): a reading with a percentage beats one without.
void MergeInto(std::vector<BatteryReading>& list, BatteryReading r);

}  // namespace bh
