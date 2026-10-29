#!/usr/bin/env bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
export PATH="${SCRIPT_DIR}/cross/bin:${PATH}"

cd ${SCRIPT_DIR}/mlibc && MSYSTEM=MSYS meson \
    setup \
    --cross-file=${SCRIPT_DIR}/x86-64.cross-file \
    --prefix=/usr \
    -Dheaders_only=true \
    -Dlinux_kernel_headers=${SCRIPT_DIR}/cross/linux-headers \
    --reconfigure \
    headers-build && MSYSTEM=MSYS DESTDIR=${SCRIPT_DIR}/cross ninja -C headers-build install || exit 1

INC=${SCRIPT_DIR}/cross/usr/include
find ${SCRIPT_DIR}/mlibc -path '*/include/*' -type f -size -200c ! -path '*/headers-build/*' | while read -r srcfile; do
    grep -qxE '(\.\./)+[A-Za-z0-9_./-]+' "$srcfile" || continue
    dest=$INC/${srcfile#*/include/}
    [ -f "$dest" ] && cmp -s "$srcfile" "$dest" || continue
    target=$(dirname "$srcfile")/$(cat "$srcfile")
    [ -f "$target" ] && cp "$target" "$dest"
done
