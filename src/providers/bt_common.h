// Shared helpers for the two Bluetooth providers. Both key devices by their
// PnP ContainerId ("bt:{guid}") so a device seen by both is merged into one.
#pragma once
#include <windows.h>
#include <setupapi.h>
#include <devpropdef.h>

#include <string>

namespace bh {

// Property keys written out literally to avoid INITGUID/devpkey.h differences
// between the Windows SDK and MinGW headers.
static const DEVPROPKEY kPkeyBluetoothBattery = {  // BYTE 0..100, set by the BT stack
    {0x104EA319, 0x6EE2, 0x4701, {0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5}}, 2};
static const DEVPROPKEY kPkeyContainerId = {
    {0x8C7ED206, 0x3F8A, 0x4827, {0xB3, 0xAB, 0xAE, 0x9E, 0x1F, 0xAE, 0xFC, 0x6C}}, 2};
static const DEVPROPKEY kPkeyFriendlyName = {
    {0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};
static const DEVPROPKEY kPkeyDeviceDesc = {
    {0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 2};
static const DEVPROPKEY kPkeyBtClassOfDevice = {  // UINT32 on BTHENUM\DEV_ nodes
    {0x2BD67D8B, 0x8BEB, 0x48D5, {0x87, 0xE0, 0x6C, 0xDA, 0x34, 0x28, 0x04, 0x0A}}, 10};
static const DEVPROPKEY kPkeyIsConnected = {  // BOOLEAN, Bluetooth device nodes
    {0x83DA6326, 0x97A6, 0x4088, {0x94, 0x53, 0xA1, 0x92, 0x3F, 0x57, 0x3B, 0x29}}, 15};

inline std::wstring GuidToString(const GUID& g) {
    wchar_t s[64];
    _snwprintf(s, 64, L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", (unsigned long)g.Data1,
               g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5],
               g.Data4[6], g.Data4[7]);
    return s;
}

inline bool GetPropGuid(HDEVINFO set, SP_DEVINFO_DATA* dev, const DEVPROPKEY& key, GUID* out) {
    DEVPROPTYPE type;
    return SetupDiGetDevicePropertyW(set, dev, &key, &type, (PBYTE)out, sizeof(GUID), nullptr, 0) &&
           type == DEVPROP_TYPE_GUID;
}

inline std::wstring GetPropString(HDEVINFO set, SP_DEVINFO_DATA* dev, const DEVPROPKEY& key) {
    DEVPROPTYPE type;
    wchar_t buf[256];
    if (SetupDiGetDevicePropertyW(set, dev, &key, &type, (PBYTE)buf, sizeof(buf), nullptr, 0) &&
        type == DEVPROP_TYPE_STRING)
        return buf;
    return {};
}

inline bool GetPropU32(HDEVINFO set, SP_DEVINFO_DATA* dev, const DEVPROPKEY& key, unsigned* out) {
    DEVPROPTYPE type;
    BYTE buf[8] = {};
    if (!SetupDiGetDevicePropertyW(set, dev, &key, &type, buf, sizeof(buf), nullptr, 0)) return false;
    if (type == DEVPROP_TYPE_BYTE) *out = buf[0];
    else if (type == DEVPROP_TYPE_UINT32) *out = *(UINT32*)buf;
    else if (type == DEVPROP_TYPE_BOOLEAN) *out = buf[0] ? 1 : 0;
    else return false;
    return true;
}

// The "computer itself" container that every internal device belongs to.
inline bool IsLocalMachineContainer(const std::wstring& c) {
    return _wcsicmp(c.c_str(), L"{00000000-0000-0000-FFFF-FFFFFFFFFFFF}") == 0;
}

}  // namespace bh
