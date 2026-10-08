#!/usr/bin/env bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
export PATH="${SCRIPT_DIR}/cross/bin:${PATH}"

cd ${SCRIPT_DIR}/mlibc && meson \
    setup \
    --cross-file=${SCRIPT_DIR}/x86-64.cross-file \
    --prefix=/usr \
    -Dheaders_only=true \
    --reconfigure \
    headers-build && DESTDIR=${SCRIPT_DIR}/cross ninja -C headers-build install
