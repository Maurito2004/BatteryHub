// Console output for --dump. The app is a GUI-subsystem binary, so output is
// attached to the parent console (cmd/PowerShell) or written to a redirected
// stdout (`BatteryHub.exe --dump > dump.txt`) as UTF-8.
#pragma once

namespace bh {

extern bool g_verbose;  // true while running --dump: providers log details

bool OpenConsoleForOutput();
void Out(const wchar_t* fmt, ...);  // use %ls for wide strings

}  // namespace bh

#define DLOG(...)                          \
    do {                                   \
        if (::bh::g_verbose) ::bh::Out(__VA_ARGS__); \
    } while (0)
