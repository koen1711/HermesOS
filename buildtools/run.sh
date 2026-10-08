#!/bin/sh
BINARY_DIR=$1
sh ./create_image.sh "$BINARY_DIR"
qemu-system-x86_64 -machine pc -s -kernel "$BINARY_DIR/OS" -drive file="$BINARY_DIR/core.img",format=raw,if=ide -serial stdio -device bochs-display -rtc base=localtime -m 4G