#!/bin/sh

set -e

BINARY_DIR=$1
if [ -z "$BINARY_DIR" ]; then
    echo "Usage: $0 <BINARY_DIR>" >&2
    exit 1
fi

BASE_IMG=$BINARY_DIR/base.img
CORE_IMG=$BINARY_DIR/core.img
ISODIR=$BINARY_DIR/isodir
GRUB_CACHE=$BINARY_DIR/grub-cache

IMG_SIZE=16G

FAT_SERIAL=48455253
FAT_LABEL="EFI SYSTEM"

for tool in mformat mcopy mmd sfdisk grub-mkstandalone; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: '$tool' not found (install mtools, fdisk and grub)." >&2
        exit 1
    fi
done

if [ ! -f "$BASE_IMG" ] || [ ! -d "$GRUB_CACHE/grub" ]; then
    echo "Creating base image with BIOS GRUB (one-time, needs sudo)"
    rm -f "$BASE_IMG"
    rm -rf "$GRUB_CACHE"
    truncate -s "$IMG_SIZE" "$BASE_IMG"
    parted "$BASE_IMG" --script -- \
            mklabel gpt \
            mkpart primary fat32 1MiB 15GiB \
            mkpart primary 15GiB 15362MiB \
            set 1 boot on \
            set 1 esp on \
            set 2 bios_grub on

    LOOP_DEVICE=$(sudo losetup --find --show --partscan "$BASE_IMG")
    MNT=$BINARY_DIR/TEMP_MNT
    cleanup() {
        sudo umount "$MNT" 2>/dev/null || true
        rmdir "$MNT" 2>/dev/null || true
        sudo losetup -d "$LOOP_DEVICE" 2>/dev/null || true
    }
    trap cleanup EXIT

    sudo mkfs.vfat -F 32 -i "$FAT_SERIAL" -n "$FAT_LABEL" "$LOOP_DEVICE"p1
    mkdir -p "$MNT"
    sudo mount "$LOOP_DEVICE"p1 "$MNT"
    sudo grub-install --target=i386-pc --install-modules="multiboot2" \
            --root-directory="$MNT" --no-floppy --boot-directory="$MNT"/boot "$LOOP_DEVICE"

    mkdir -p "$GRUB_CACHE"
    cp -r "$MNT"/boot/grub "$GRUB_CACHE"/grub
    sudo umount "$MNT"
    rmdir "$MNT"
    sudo losetup -d "$LOOP_DEVICE"
    trap - EXIT
fi

PART1=$(sfdisk -d "$BASE_IMG" | grep -E '1 *: *start=')
PART_START=$(echo "$PART1" | sed -E 's/.*start= *([0-9]+).*/\1/')
PART_SIZE=$(echo "$PART1" | sed -E 's/.*size= *([0-9]+).*/\1/')
PART_OFFSET=$((PART_START * 512))
FAT="$CORE_IMG@@$PART_OFFSET"

rm -f "$CORE_IMG"
cp --reflink=auto --sparse=always "$BASE_IMG" "$CORE_IMG"

mformat -i "$FAT" -F -N "$FAT_SERIAL" -v "$FAT_LABEL" \
        -T "$PART_SIZE" -h 64 -s 32 -H "$PART_START" ::

mcopy -i "$FAT" -s -Q -D o "$ISODIR"/* ::/
mmd -i "$FAT" -D s ::/boot ::/boot/grub ::/EFI ::/EFI/BOOT 2>/dev/null || true
mcopy -i "$FAT" -s -Q -D o "$GRUB_CACHE"/grub/* ::/boot/grub/

GRUB_CFG=$ISODIR/boot/grub/grub.cfg
EFI_BIN=$GRUB_CACHE/BOOTX64.efi
if [ ! -f "$EFI_BIN" ] || ! cmp -s "$GRUB_CFG" "$GRUB_CACHE/grub.cfg.efi"; then
    grub-mkstandalone -O x86_64-efi -o "$EFI_BIN" --modules="part_gpt multiboot2 fat" \
            "boot/grub/grub.cfg=$GRUB_CFG"
    cp "$GRUB_CFG" "$GRUB_CACHE/grub.cfg.efi"
fi
mcopy -i "$FAT" -Q -D o "$EFI_BIN" ::/EFI/BOOT/BOOTX64.efi
