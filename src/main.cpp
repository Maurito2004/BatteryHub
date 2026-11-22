// BatteryHub — wireless peripheral battery levels in the Windows notification area.
//   BatteryHub.exe           run in the tray
//   BatteryHub.exe --dump    print every HID / Bluetooth / XInput device and its battery
#include <windows.h>
#include <shellapi.h>

#include "../assets/resource.h"
#include <cwchar>
#include <vector>

#include "core/log.h"
#include "core/provider.h"
#include "ui/app.h"
#include "ui/icon_renderer.h"

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool dump = false, help = false;
    for (int i = 1; argv && i < argc; ++i) {
        if (!lstrcmpiW(argv[i], L"--preview") && i + 1 < argc) {  // dev aid: render sample icons
            bh::IconRendererInit();
            bool ok = bh::RenderPreviewPng(argv[i + 1]);
            bh::IconRendererShutdown();
            LocalFree(argv);
            return ok ? 0 : 1;
        }
        if (!lstrcmpiW(argv[i], L"--probe") && i + 3 < argc) {  // --probe VID PID PAGE [bytes...]
            bh::OpenConsoleForOutput();
            auto hex = [](const wchar_t* s) { return (unsigned)wcstoul(s, nullptr, 16); };
            std::vector<uint8_t> bytes;
            for (int k = i + 4; k < argc; ++k) bytes.push_back((uint8_t)hex(argv[k]));
            int rc = bh::RunHidProbe((uint16_t)hex(argv[i + 1]), (uint16_t)hex(argv[i + 2]),
                                     (uint16_t)hex(argv[i + 3]), bytes);
            LocalFree(argv);
            return rc;
        }
        if (!lstrcmpiW(argv[i], L"--dump") || !lstrcmpiW(argv[i], L"/dump")) dump = true;
        else if (!lstrcmpiW(argv[i], L"--help") || !lstrcmpiW(argv[i], L"-h") || !lstrcmpiW(argv[i], L"/?"))
            help = true;
    }
    if (argv) LocalFree(argv);

    if (help) {
        bh::OpenConsoleForOutput();
        bh::Out(L"BatteryHub %ls\n", BH_VERSION_WSTR);
        bh::Out(L"  BatteryHub.exe            Ejecuta en el área de notificación");
        bh::Out(L"  BatteryHub.exe --dump     Lista HID / Bluetooth / XInput con VID, PID, UsagePage,");
        bh::Out(L"                            Usage y el porcentaje leído (o el error)");
        bh::Out(L"  BatteryHub.exe --dump > dump.txt   Guarda el diagnóstico en un archivo (UTF-8)");
        return 0;
    }
    if (dump) return bh::RunDump();
    return bh::RunTrayApp(inst);
}
