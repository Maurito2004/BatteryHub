#pragma once
#include <windows.h>

namespace bh {

// Runs the tray application until "Salir". Returns the process exit code.
// afterUpdate: started by the updater; waits for the previous version to exit.
int RunTrayApp(HINSTANCE inst, bool afterUpdate = false);

// `--dump`: prints every HID / Bluetooth / XInput device and what was read.
int RunDump();

}  // namespace bh
