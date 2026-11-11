#!/usr/bin/env bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
export PATH="${SCRIPT_DIR}/cross/bin:${PATH}"

GCC_INCLUDE=$(x86_64-hermes-gcc -print-file-name=include)

BUILD_DIR=build
DESTDIR=${SCRIPT_DIR}/cross
EXTRA_CPP_ARGS=""
if [ -n "${UNIKERNEL}" ]; then
    BUILD_DIR=build-unikernel
    DESTDIR=${SCRIPT_DIR}/cross-unikernel
    EXTRA_CPP_ARGS="-DHERMES_UNIKERNEL -idirafter ${SCRIPT_DIR}/../include"
fi

cd ${SCRIPT_DIR}/mlibc && meson \
    setup \
    --cross-file=${SCRIPT_DIR}/x86-64.cross-file \
    --prefix=/usr \
    -Ddefault_library=both \
    -Dno_headers=true \
    -Dlinux_kernel_headers=${SCRIPT_DIR}/cross/linux-headers \
    -Dc_args="-isystem ${GCC_INCLUDE}" \
    -Dcpp_args="-isystem ${GCC_INCLUDE} -D_GNU_SOURCE ${EXTRA_CPP_ARGS}" \
    --reconfigure \
    ${BUILD_DIR} && DESTDIR=${DESTDIR} ninja -C ${BUILD_DIR} install
