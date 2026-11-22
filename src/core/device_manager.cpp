#include "device_manager.h"

#include <windows.h>

#include "log.h"
#include "settings.h"

namespace bh {

void DeviceManager::EnablePersistence() {
    persist_ = true;
    for (auto& r : LoadLastKnown()) {
        TrackedDevice t;
        t.r = r;
        t.stale = true;
        t.present = false;  // shown only once a poll finds it (e.g. its dongle is plugged)
        devices_.push_back(t);
    }
}

DeviceManager::DeviceManager() {
    // Order matters only for --dump readability; ids keep results distinct.
    providers_.push_back(CreateHidProvider());
    providers_.push_back(CreateXInputProvider());
    providers_.push_back(CreateBluetoothPnpProvider());
    providers_.push_back(CreateBleGattProvider());
}

void MergeInto(std::vector<BatteryReading>& list, BatteryReading r) {
    for (auto& e : list) {
        if (e.id != r.id) continue;
        bool rBetter = (r.percent >= 0 && e.percent < 0) || (r.online && !e.online && r.percent >= 0);
        if (rBetter) {
            if (r.name.empty()) r.name = e.name;
            if (r.kind == DeviceKind::Unknown) r.kind = e.kind;
            e = std::move(r);
        } else {
            if (e.kind == DeviceKind::Unknown) e.kind = r.kind;
            if (e.charge == Charge::Unknown) e.charge = r.charge;
        }
        return;
    }
    list.push_back(std::move(r));
}

std::vector<BatteryReading> DeviceManager::PollAll() {
    std::vector<BatteryReading> all;
    for (auto& p : providers_) {
        std::vector<BatteryReading> part;
        DLOG(L"");
        DLOG(L"==== %ls ====", p->Name());
        p->Poll(part);
        for (auto& r : part) {
            r.source = p->Name();
            MergeInto(all, std::move(r));
        }
    }
    // A GameSir pad in XInput mode would also show up as "Xbox Controller N" if its
    // dongle ever reported a battery type; the vendor reading is the accurate one.
    bool vendorPad = false;
    for (const auto& r : all) vendorPad |= r.id.rfind(L"gamesir:", 0) == 0;
    if (vendorPad) {
        for (size_t i = all.size(); i-- > 0;)
            if (all[i].id.rfind(L"xinput:", 0) == 0) all.erase(all.begin() + i);
    }
    return all;
}

void DeviceManager::Apply(std::vector<BatteryReading> fresh) {
    const auto now = GetTickCount64();
    std::vector<bool> seen(devices_.size(), false);

    for (auto& r : fresh) {
        size_t i = 0;
        for (; i < devices_.size(); ++i)
            if (devices_[i].r.id == r.id) break;

        if (i == devices_.size()) {
            // Never report a brand-new device that has no data at all.
            if (r.percent < 0 && !r.online) continue;
            TrackedDevice t;
            t.r = r;
            t.stale = !r.online || (r.percent < 0 && r.charge != Charge::Charging);
            t.present = true;
            t.lastSeen = now;
            devices_.push_back(t);
            seen.push_back(true);
            if (persist_ && r.online && r.percent >= 0) SaveLastKnown(r);
            continue;
        }

        seen[i] = true;
        auto& t = devices_[i];
        t.present = true;
        if (r.percent >= 0 && r.online) {
            if (persist_ && (t.r.percent != r.percent || t.stale)) SaveLastKnown(r);  // only on change
            t.r = r;
            t.stale = false;
            t.lastSeen = now;
        } else {
            // Asleep / disconnected / unreadable: keep last known percentage.
            int last = t.r.percent;
            Charge lastCharge = t.r.charge;
            std::wstring name = t.r.name;
            t.r = r;
            if (t.r.percent < 0) t.r.percent = last;
            if (t.r.name.empty()) t.r.name = name;
            if (!r.online) t.r.charge = lastCharge;
            // "Charging, level unknown" (AULA on cable, Nova Lite) is live, not stale.
            t.stale = !r.online || (r.percent < 0 && r.charge != Charge::Charging);
        }
    }

    for (size_t i = 0; i < seen.size() && i < devices_.size(); ++i) {
        if (!seen[i]) {  // gone from Windows: unplugged, switched off, out of range
            devices_[i].r.online = false;
            devices_[i].stale = true;
            devices_[i].present = false;
        }
    }
}

}  // namespace bh
