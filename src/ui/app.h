#pragma once
#include <windows.h>

namespace bh {

// Runs the tray application until "Salir". Returns the process exit code.
int RunTrayApp(HINSTANCE inst);

// `--dump`: prints every HID / Bluetooth / XInput device and what was read.
int RunDump();

}  // namespace bh
