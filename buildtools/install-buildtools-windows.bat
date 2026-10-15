@echo off
setlocal enabledelayedexpansion

rem Build the x86_64-hermes cross-compiler on Windows by handing off to the
rem existing install-buildtools-linux.sh under a Unix-like shell (MSYS2).
rem
rem Prereqs (one-time, inside MSYS2):
rem   pacman -Syu
rem   pacman -S --needed base-devel mingw-w64-x86_64-toolchain curl tar mtools mingw-w64-x86_64-gptfdisk

set "SCRIPT_DIR=%~dp0"

set "BASH_EXE="
if exist "C:\msys64\usr\bin\bash.exe"      set "BASH_EXE=C:\msys64\usr\bin\bash.exe"
if not defined BASH_EXE if exist "C:\tools\msys64\usr\bin\bash.exe" set "BASH_EXE=C:\tools\msys64\usr\bin\bash.exe"
if not defined BASH_EXE for /f "delims=" %%I in ('where bash 2^>nul') do (
    if not defined BASH_EXE set "BASH_EXE=%%I"
)

if not defined BASH_EXE (
    echo ERROR: No bash shell found.
    echo.
    echo Install MSYS2 from https://www.msys2.org/ then run inside the MSYS2 shell:
    echo     pacman -Syu
    echo     pacman -S --needed base-devel mingw-w64-x86_64-toolchain curl tar mtools mingw-w64-x86_64-gptfdisk
    echo.
    echo Re-run this script afterwards.
    exit /b 1
)

if not defined THREADS set "THREADS=%NUMBER_OF_PROCESSORS%"

rem Build with the native MinGW toolchain so the resulting .exe files don't
rem depend on msys-2.0.dll (CLion/CMake run them outside MSYS2).
set "MSYSTEM=MINGW64"
set "CHERE_INVOKING=1"
rem GCC 15+ host compilers default to C23, which breaks older binutils sources.
set "CFLAGS=-g -O2 -std=gnu17"
rem These GCC defines are paths inside the target sysroot; stop MSYS2 from
rem rewriting /usr/include into C:/msys64/usr/include when passing them to gcc.
set "MSYS2_ARG_CONV_EXCL=-DNATIVE_SYSTEM_HEADER_DIR=;-DLOCAL_INCLUDE_DIR="

echo Using bash: %BASH_EXE%
echo Threads:    %THREADS%

"%BASH_EXE%" -lc "cd '%SCRIPT_DIR:\=/%' && THREADS=%THREADS% sh ./install-buildtools-linux.sh && bash ./bundle-dlls.sh"
exit /b %ERRORLEVEL%
