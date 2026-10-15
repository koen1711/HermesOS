#!/usr/bin/env bash
# Copy the MinGW runtime DLLs the cross toolchain links against next to its
# executables. Windows looks in an exe's own directory before PATH, so the
# toolchain keeps working when another MinGW (e.g. CLion's bundled one) comes
# first in PATH and ships older, incompatible copies of the same DLLs.
# Run from an MSYS2 MINGW64 shell.

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

find "${SCRIPT_DIR}/cross" -name '*.exe' -type f | while read -r exe; do
    dir=$(dirname "$exe")
    ldd "$exe" 2>/dev/null | awk '$3 ~ "^/mingw64/bin/" { print $3 }' | while read -r dll; do
        [ -f "$dir/$(basename "$dll")" ] || cp "$dll" "$dir/"
    done
done
