cmake_minimum_required(VERSION 3.16)
project(BatteryHub VERSION 1.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE MinSizeRel)
endif()

add_executable(BatteryHub WIN32
  src/main.cpp
  src/core/device.cpp
  src/core/log.cpp
  src/core/settings.cpp
  src/core/hid_util.cpp
  src/core/device_manager.cpp
  src/providers/bluetooth_pnp_provider.cpp
  src/providers/ble_gatt_provider.cpp
  src/providers/xinput_provider.cpp
  src/providers/hid_provider.cpp
  src/providers/hid_models.cpp
  src/providers/logitech_hidpp.cpp
  src/providers/razer.cpp
  src/providers/headsets.cpp
  src/providers/playstation.cpp
  src/providers/compx.cpp
  src/providers/gamesir.cpp
  src/ui/icon_renderer.cpp
  src/ui/tray.cpp
  src/ui/taskbar_widget.cpp
  src/ui/app.cpp
  assets/app.rc
)

target_compile_definitions(BatteryHub PRIVATE UNICODE _UNICODE
  _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000000)  # Windows 10+ APIs (BLE GATT)
target_include_directories(BatteryHub PRIVATE assets)
# xinput1_4.dll is loaded at runtime (no import), so it is not linked here.
target_link_libraries(BatteryHub PRIVATE
  hid setupapi comctl32 cfgmgr32 bluetoothapis gdiplus shell32 user32 gdi32 advapi32 ole32)

if(MSVC)
  set_property(TARGET BatteryHub PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")  # static CRT: portable exe
  target_compile_options(BatteryHub PRIVATE /utf-8 /W3 /GS- /Gy /Gw)
  target_link_options(BatteryHub PRIVATE /OPT:REF /OPT:ICF /MANIFEST:NO)
else()
  target_compile_options(BatteryHub PRIVATE -municode -fno-rtti -ffunction-sections -fdata-sections
                         -Wall -Wno-unknown-pragmas -Wno-missing-field-initializers)
  target_link_options(BatteryHub PRIVATE -municode -static -s -Wl,--gc-sections)
endif()
