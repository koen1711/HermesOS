#!/bin/sh
set -e
BINARY_DIR=$1
python3 "$(dirname "$0")/disk-mngr/disk_mngr.py" "$BINARY_DIR"
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

DISK_IF=ide
[ -n "$VIRTIO" ] && DISK_IF=virtio
echo "Disk interface: $DISK_IF"

qemu-system-x86_64 -machine pc $ACCEL -netdev user,id=net0,hostfwd=tcp:127.0.0.1:8000-:8000 -device ${QEMU_NIC:-e1000},netdev=net0 -smp "$SMP" -s -drive file="$BINARY_DIR/core.img",format=raw,if=$DISK_IF -serial stdio -device bochs-display -rtc base=localtime -m "${QEMU_MEMORY:-4G}" $QEMU_EXTRA_ARGS
