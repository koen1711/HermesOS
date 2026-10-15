#!/usr/bin/env bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
export PATH="${SCRIPT_DIR}/cross/bin:${PATH}"

# On MSYS2, meson is an msys-python package and refuses to run when MSYSTEM is MINGW64.
# headers_only compiles nothing, so running it as MSYS is fine (no-op elsewhere).
cd ${SCRIPT_DIR}/mlibc && MSYSTEM=MSYS meson \
    setup \
    --cross-file=${SCRIPT_DIR}/x86-64.cross-file \
    --prefix=/usr \
    -Dheaders_only=true \
    --reconfigure \
    headers-build && MSYSTEM=MSYS DESTDIR=${SCRIPT_DIR}/cross ninja -C headers-build install || exit 1

# Without symlink support (e.g. git on Windows), mlibc's symlinked headers are
# checked out as text files holding the link target, and get installed as-is.
# Replace each installed placeholder with the file its source symlink points to.
INC=${SCRIPT_DIR}/cross/usr/include
find ${SCRIPT_DIR}/mlibc -path '*/include/*' -type f -size -200c ! -path '*/headers-build/*' | while read -r srcfile; do
    grep -qxE '(\.\./)+[A-Za-z0-9_./-]+' "$srcfile" || continue
    dest=$INC/${srcfile#*/include/}
    [ -f "$dest" ] && cmp -s "$srcfile" "$dest" || continue
    target=$(dirname "$srcfile")/$(cat "$srcfile")
    [ -f "$target" ] && cp "$target" "$dest"
done
