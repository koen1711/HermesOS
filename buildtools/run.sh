#!/bin/sh
set -e
BINARY_DIR=$1
sh ./create_image.sh "$BINARY_DIR"
ACCEL=""
[ -w /dev/kvm ] && ACCEL="-enable-kvm -cpu host"

echo "Booting HermesOS with QEMU..."
echo "Acceleration: $ACCEL"

if [ -z "$SMP" ]; then
    HOST_CPUS=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
    SMP=$((HOST_CPUS / 2))
    [ "$SMP" -lt 1 ] && SMP=1
    [ "$SMP" -gt 16 ] && SMP=16
fi
echo "Booting with $SMP CPUs"

qemu-system-x86_64 -machine pc $ACCEL -smp "$SMP" -s -drive file="$BINARY_DIR/core.img",format=raw,if=ide -serial stdio -device bochs-display -rtc base=localtime -m 4G
