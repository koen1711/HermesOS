@echo off
setlocal enabledelayedexpansion

rem Windows entry point for building core.img.
rem Hands off to create_image_windows.sh under MSYS2 bash.

set "SCRIPT_DIR=%~dp0"
set "BINARY_DIR=%~1"
if "%BINARY_DIR%"=="" (
    echo Usage: %~nx0 ^<BINARY_DIR^>
    exit /b 1
)

set "BASH_EXE="
if exist "C:\msys64\usr\bin\bash.exe"      set "BASH_EXE=C:\msys64\usr\bin\bash.exe"
if not defined BASH_EXE if exist "C:\tools\msys64\usr\bin\bash.exe" set "BASH_EXE=C:\tools\msys64\usr\bin\bash.exe"
if not defined BASH_EXE for /f "delims=" %%I in ('where bash 2^>nul') do (
    if not defined BASH_EXE set "BASH_EXE=%%I"
)

if not defined BASH_EXE (
    echo ERROR: No bash shell found.
    echo Install MSYS2 from https://www.msys2.org/ then run inside the MSYS2 shell:
    echo     pacman -Syu
    echo     pacman -S --needed mtools mingw-w64-x86_64-gptfdisk
    exit /b 1
)

rem MSYS2 bash wants forward-slash paths.
set "SCRIPT_DIR_MSYS=%SCRIPT_DIR:\=/%"
set "BINARY_DIR_MSYS=%BINARY_DIR:\=/%"

"%BASH_EXE%" -lc "export PATH=/mingw64/bin:/usr/bin:$PATH && cd '%SCRIPT_DIR_MSYS%' && sh ./create_image_windows.sh '%BINARY_DIR_MSYS%'"
exit /b %ERRORLEVEL%
