// One notification-area icon per device. Icons are only re-rendered when what
// they show changes, and are re-added when Explorer restarts (TaskbarCreated).
#pragma once
#include <windows.h>

#include <string>
#include <vector>

#include "icon_renderer.h"

namespace bh {

struct TrayItem {
    std::wstring id;
    IconSpec spec;
    std::wstring tip;
};

class Tray {
public:
    void Init(HWND hwnd, UINT callbackMsg) {
        hwnd_ = hwnd;
        msg_ = callbackMsg;
    }
    void Sync(const std::vector<TrayItem>& items);
    void ReAddAll();   // after Explorer restart
    void RemoveAll();

    // id of the icon that sent a callback (uID -> device id)
    std::wstring IdForUid(UINT uid) const;

private:
    struct Entry {
        std::wstring id;
        UINT uid = 0;
        IconSpec spec;
        std::wstring tip;
        HICON icon = nullptr;
    };
    bool Add(Entry& e);
    void Modify(Entry& e);
    void Remove(Entry& e);

    HWND hwnd_ = nullptr;
    UINT msg_ = 0;
    std::vector<Entry> entries_;
};

}  // namespace bh
