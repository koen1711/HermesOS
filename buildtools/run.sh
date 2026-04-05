#!/bin/sh

BINARY_DIR=$1

sh ./create_image.sh "$BINARY_DIR"

qemu-system-x86_64 -s -hda "$BINARY_DIR"/core.img -serial stdio -rtc base=localtime -m 4G