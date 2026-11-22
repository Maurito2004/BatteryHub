#include "tray.h"

#include <shellapi.h>

namespace bh {

static void FillTip(NOTIFYICONDATAW& nid, const std::wstring& tip) {
    wcsncpy(nid.szTip, tip.c_str(), ARRAYSIZE(nid.szTip) - 1);
    nid.szTip[ARRAYSIZE(nid.szTip) - 1] = 0;
}

bool Tray::Add(Entry& e) {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = e.uid;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    nid.uCallbackMessage = msg_;
    nid.hIcon = e.icon;
    FillTip(nid, e.tip);
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) return false;
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    return true;
}

void Tray::Modify(Entry& e) {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = e.uid;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.hIcon = e.icon;
    FillTip(nid, e.tip);
    if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) Add(e);  // icon lost (Explorer restarted)
}

void Tray::Remove(Entry& e) {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = e.uid;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (e.icon) DestroyIcon(e.icon);
    e.icon = nullptr;
}

void Tray::Sync(const std::vector<TrayItem>& items) {
    // If the set or order of devices changed, rebuild so the taskbar order is stable.
    bool sameSet = items.size() == entries_.size();
    for (size_t i = 0; sameSet && i < items.size(); ++i) sameSet = items[i].id == entries_[i].id;

    if (!sameSet) {
        std::vector<Entry> old;
        old.swap(entries_);
        for (auto& e : old) Remove(e);
        for (const auto& it : items) {
            Entry e;
            e.id = it.id;
            // Stable per-device uID (hash of the id): Windows remembers "show on the
            // taskbar" per (exe, uID), so the user's choice survives restarts.
            UINT h = 2166136261u;
            for (wchar_t c : it.id) h = (h ^ (UINT)c) * 16777619u;
            e.uid = 1 + h % 60000;
            for (bool clash = true; clash;) {
                clash = false;
                for (const auto& o : entries_)
                    if (o.uid == e.uid) {
                        e.uid = e.uid % 60000 + 1;
                        clash = true;
                    }
            }
            e.spec = it.spec;
            e.tip = it.tip;
            e.icon = RenderTrayIcon(it.spec);
            Add(e);
            entries_.push_back(e);
        }
        return;
    }
    for (size_t i = 0; i < items.size(); ++i) {
        Entry& e = entries_[i];
        if (e.spec == items[i].spec && e.tip == items[i].tip) continue;
        if (e.spec != items[i].spec) {
            HICON old = e.icon;
            e.icon = RenderTrayIcon(items[i].spec);
            e.spec = items[i].spec;
            e.tip = items[i].tip;
            Modify(e);
            if (old) DestroyIcon(old);
        } else {
            e.tip = items[i].tip;
            Modify(e);
        }
    }
}

void Tray::ReAddAll() {
    for (auto& e : entries_) Add(e);
}

void Tray::RemoveAll() {
    for (auto& e : entries_) Remove(e);
    entries_.clear();
}

std::wstring Tray::IdForUid(UINT uid) const {
    for (const auto& e : entries_)
        if (e.uid == uid) return e.id;
    return {};
}

}  // namespace bh
