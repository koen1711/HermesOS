#!/usr/bin/env bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
export PATH="${SCRIPT_DIR}/cross/bin:${PATH}"

GCC_INCLUDE=$(x86_64-hermes-gcc -print-file-name=include)

cd ${SCRIPT_DIR}/mlibc && meson \
    setup \
    --cross-file=${SCRIPT_DIR}/x86-64.cross-file \
    --prefix=/usr \
    -Ddefault_library=static \
    -Dno_headers=true \
    -Dc_args="-isystem ${GCC_INCLUDE}" \
    -Dcpp_args="-isystem ${GCC_INCLUDE} -D_GNU_SOURCE" \
    --reconfigure \
    build && DESTDIR=${SCRIPT_DIR}/cross ninja -C build install
