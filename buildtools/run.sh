#!/bin/sh
BINARY_DIR=$1
sh ./create_image.sh "$BINARY_DIR"
ACCEL=""
[ -w /dev/kvm ] && ACCEL="-enable-kvm -cpu host"
qemu-system-x86_64 -machine pc $ACCEL -smp 4 -s -kernel "$BINARY_DIR/OS" -drive file="$BINARY_DIR/core.img",format=raw,if=ide -serial stdio -device bochs-display -rtc base=localtime -m 4G
