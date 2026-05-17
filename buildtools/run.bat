@echo off
setlocal enabledelayedexpansion

rem Windows equivalent of run.sh.
rem
rem Builds core.img via create_image.bat (mtools+sgdisk under MSYS2 — no
rem losetup/mount/sudo needed), then boots the kernel via QEMU's -kernel
rem (multiboot direct load, skips needing grub-install) with the disk
rem attached as -hda so the kernel's ATA+GPT driver finds it.
rem
rem Usage:  run.bat <BINARY_DIR>
rem   <BINARY_DIR> must contain the built kernel ELF (named "OS").

set "SCRIPT_DIR=%~dp0"
set "BINARY_DIR=%~1"
if "%BINARY_DIR%"=="" (
    echo Usage: %~nx0 ^<BINARY_DIR^>
    exit /b 1
)

set "KERNEL=%BINARY_DIR%\OS"
if not exist "%KERNEL%" (
    echo ERROR: kernel not found at "%KERNEL%"
    echo Build the OS target first.
    exit /b 1
)

call "%SCRIPT_DIR%create_image.bat" "%BINARY_DIR%"
if errorlevel 1 (
    echo ERROR: create_image.bat failed.
    exit /b 1
)

set "QEMU="
if exist "C:\Program Files\qemu\qemu-system-x86_64.exe" set "QEMU=C:\Program Files\qemu\qemu-system-x86_64.exe"
if not defined QEMU if exist "C:\Program Files (x86)\qemu\qemu-system-x86_64.exe" set "QEMU=C:\Program Files (x86)\qemu\qemu-system-x86_64.exe"
if not defined QEMU for /f "delims=" %%I in ('where qemu-system-x86_64 2^>nul') do (
    if not defined QEMU set "QEMU=%%I"
)

if not defined QEMU (
    echo ERROR: qemu-system-x86_64.exe not found.
    echo Install QEMU from https://www.qemu.org/download/#windows and either:
    echo   - accept the default install path, or
    echo   - add the QEMU install dir to PATH.
    exit /b 1
)

echo Using QEMU: %QEMU%
echo Booting:    %KERNEL%
echo Disk:       %BINARY_DIR%\core.img

"%QEMU%" -s -kernel "%KERNEL%" -hda "%BINARY_DIR%\core.img" -serial stdio -device bochs-display -rtc base=localtime -m 4G
exit /b %ERRORLEVEL%
