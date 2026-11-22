#include "log.h"

#include <windows.h>
#include <cstdarg>
#include <cwchar>

namespace bh {

bool g_verbose = false;
static HANDLE g_out = nullptr;
static bool g_isConsole = false;

bool OpenConsoleForOutput() {
    // A redirected stdout (file or pipe) is inherited even by GUI apps.
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN) {
        g_out = h;
        g_isConsole = GetConsoleMode(h, &mode) != 0;
        return true;
    }
    if (!AttachConsole(ATTACH_PARENT_PROCESS) && !AllocConsole()) return false;
    g_out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_WRITE, nullptr,
                        OPEN_EXISTING, 0, nullptr);
    g_isConsole = true;
    if (g_out != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteConsoleW(g_out, L"\r\n", 2, &w, nullptr);  // start below the shell prompt
    }
    return g_out != INVALID_HANDLE_VALUE;
}

void Out(const wchar_t* fmt, ...) {
    if (!g_out || g_out == INVALID_HANDLE_VALUE) return;
    wchar_t buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnwprintf(buf, 4094, fmt, ap);
    va_end(ap);
    if (n < 0) n = 4094;
    buf[n++] = L'\n';
    buf[n] = 0;
    DWORD w;
    if (g_isConsole) {
        WriteConsoleW(g_out, buf, (DWORD)n, &w, nullptr);
    } else {
        char utf8[12288];
        int len = WideCharToMultiByte(CP_UTF8, 0, buf, n, utf8, sizeof(utf8), nullptr, nullptr);
        if (len > 0) WriteFile(g_out, utf8, (DWORD)len, &w, nullptr);
    }
}

}  // namespace bh
