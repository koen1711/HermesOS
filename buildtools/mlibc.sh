#!/usr/bin/env bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

cd ${SCRIPT_DIR}/mlibc && meson \
    setup \
    --cross-file=${SCRIPT_DIR}/x86-64.cross-file \
    --prefix=/ \
    -Dheaders_only=true \
    headers-build &&
    DESTDIR=${SCRIPT_DIR}/../include/mlibc ninja -C headers-build install
