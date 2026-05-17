#!/bin/sh
set -e

# Build core.img on Windows (run under MSYS2 bash via create_image.bat).
#
# Unlike create_image.sh on Linux, this script never uses sudo/losetup/mount —
# those don't exist on Windows. Instead it operates directly on the image file
# using sgdisk (GPT) and mtools (FAT32 read/write).
#
# Trade-off vs the Linux flow: no bootloader is installed on the image. The
# Windows run.bat boots the kernel via QEMU's -kernel flag (multiboot direct
# load) and only attaches this image via -hda for the kernel's disk driver.

BINARY_DIR=$1
if [ -z "$BINARY_DIR" ]; then
    echo "Usage: $0 <BINARY_DIR>" >&2
    exit 1
fi

IMG=$BINARY_DIR/core.img

for tool in sgdisk mformat mcopy; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: '$tool' not found in MSYS2 PATH." >&2
        echo "Install with:" >&2
        echo "    pacman -S --needed mtools mingw-w64-x86_64-gptfdisk" >&2
        exit 1
    fi
done

ISODIR=$BINARY_DIR/isodir
if [ ! -d "$ISODIR" ]; then
    echo "ERROR: $ISODIR not found — build the OS target first." >&2
    exit 1
fi

# Wipe any old image so we start from a known state.
rm -f "$IMG"

# 512 MiB raw image (smaller than the Linux 1 GiB — speeds up the loop).
echo "Creating 512 MiB raw image"
dd if=/dev/zero of="$IMG" bs=1M count=512 status=none

# Write a GPT with a single FAT32 partition that starts at sector 2048 (1 MiB)
# and extends to the last usable sector. Type 0700 = Microsoft basic data,
# which is what mformat will format as FAT32.
echo "Writing GPT"
sgdisk -o "$IMG" >/dev/null
sgdisk -n 1:2048:0 -t 1:0700 -c 1:"EFI SYSTEM" "$IMG" >/dev/null

# Partition 1 byte offset = 2048 sectors * 512 bytes
PART_OFFSET=1048576

# Format partition 1 as FAT32. mtools' image@@offset syntax targets a region
# inside a file without needing the OS to mount it.
echo "Formatting partition 1 as FAT32"
mformat -i "$IMG@@$PART_OFFSET" -F -v "EFISYSTEM" ::

# Recursively copy isodir contents into the partition root.
echo "Populating partition from $ISODIR"
mcopy -i "$IMG@@$PART_OFFSET" -s -Q -o "$ISODIR"/* ::/

echo "Wrote $IMG"
