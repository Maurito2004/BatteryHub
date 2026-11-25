@echo off
rem BatteryHub build script.
rem   build.bat          release build (auto-detects MSVC cl.exe, then MinGW g++/clang++)
rem   build.bat msvc     force MSVC (run from a "Developer Command Prompt")
rem   build.bat mingw    force MinGW-w64 / LLVM-MinGW
rem Output: build\BatteryHub.exe  (single portable exe, static runtime)
setlocal enabledelayedexpansion
cd /d "%~dp0"

set SRC=src\main.cpp src\core\device.cpp src\core\log.cpp src\core\settings.cpp src\core\hid_util.cpp ^
 src\core\device_manager.cpp src\core\updater.cpp src\providers\bluetooth_pnp_provider.cpp src\providers\ble_gatt_provider.cpp ^
 src\providers\xinput_provider.cpp src\providers\hid_provider.cpp src\providers\hid_models.cpp ^
 src\providers\logitech_hidpp.cpp src\providers\razer.cpp src\providers\headsets.cpp ^
 src\providers\playstation.cpp src\providers\compx.cpp src\providers\gamesir.cpp ^
 src\ui\icon_renderer.cpp src\ui\tray.cpp src\ui\taskbar_widget.cpp src\ui\settings_window.cpp src\ui\app.cpp

set LIBS_COMMON=hid setupapi comctl32 winhttp cfgmgr32 bluetoothapis gdiplus d2d1 dwrite shell32 user32 gdi32 advapi32 ole32
if not exist build mkdir build

set MODE=%1
if "%MODE%"=="" (
    where cl >nul 2>nul && set MODE=msvc
)
if "%MODE%"=="" (
    where g++ >nul 2>nul && set MODE=mingw
)
if "%MODE%"=="" (
    rem LLVM-MinGW installed per-user with winget, not on PATH yet
    for /d %%D in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\MartinStorsjo.LLVM-MinGW*") do (
        for /d %%E in ("%%D\llvm-mingw-*") do set "PATH=%%E\bin;!PATH!"
    )
    where g++ >nul 2>nul && set MODE=mingw
)
if "%MODE%"=="" (
    echo [!] No se encontro compilador. Instala uno de:
    echo     winget install MartinStorsjo.LLVM-MinGW.UCRT
    echo     Visual Studio Build Tools ^(y usa "Developer Command Prompt"^)
    exit /b 1
)

if /i "%MODE%"=="msvc" goto :msvc
if /i "%MODE%"=="mingw" goto :mingw
echo Modo desconocido: %MODE%
exit /b 1

:msvc
echo [*] Compilando con MSVC...
rc /nologo /i assets /fo build\app.res assets\app.rc || exit /b 1
set MSLIBS=
for %%L in (%LIBS_COMMON%) do set MSLIBS=!MSLIBS! %%L.lib
cl /nologo /std:c++17 /O1 /Os /GS- /Gy /Gw /MT /EHsc /W3 /utf-8 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 /DNTDDI_VERSION=0x0A000000 ^
   /Fobuild\ /Fe:build\BatteryHub.exe %SRC% build\app.res ^
   /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /MANIFEST:NO %MSLIBS% || exit /b 1
goto :done

:mingw
echo [*] Compilando con MinGW (g++/clang++)...
windres -I assets -O coff -o build\app.res.o assets\app.rc || exit /b 1
set GLIBS=
for %%L in (%LIBS_COMMON%) do set GLIBS=!GLIBS! -l%%L
g++ -std=c++17 -Os -s -municode -mwindows -static -fno-rtti -ffunction-sections -fdata-sections ^
    -Wall -Wno-unknown-pragmas -Wno-missing-field-initializers -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=0x0A000000 ^
    -o build\BatteryHub.exe %SRC% build\app.res.o -Wl,--gc-sections %GLIBS% || exit /b 1
goto :done

:done
for %%F in (build\BatteryHub.exe) do echo [OK] build\BatteryHub.exe  (%%~zF bytes)
endlocal
