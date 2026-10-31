#!/bin/sh

BINARY_DIR=$1

if [ -f "$BINARY_DIR"/core.img ]; then
    rm "$BINARY_DIR"/core.img
fi

if  [ -f "$BINARY_DIR"/base.img ]; then
    cp "$BINARY_DIR"/base.img "$BINARY_DIR"/core.img
else
  dd if=/dev/zero of="$BINARY_DIR"/base.img bs=1M count=4096
  parted "$BINARY_DIR"/base.img --script -- \
          mklabel gpt \
          mkpart primary fat32 1MiB 3GiB \
          mkpart primary 3GiB 3074MiB \
          set 1 boot on \
          set 1 esp on \
          set 2 bios_grub on
  cp "$BINARY_DIR"/base.img "$BINARY_DIR"/core.img
fi

LOOP_DEVICE=$(sudo losetup --find --show --partscan "$BINARY_DIR"/core.img)
sudo mkfs.vfat -n "EFI SYSTEM" "$LOOP_DEVICE"p1

mkdir -p "$BINARY_DIR"/TEMP_MNT

sudo mount "$LOOP_DEVICE"p1 "$BINARY_DIR"/TEMP_MNT

sudo cp -r "$BINARY_DIR"/isodir/* "$BINARY_DIR"/TEMP_MNT/

sudo mkdir -p "$BINARY_DIR"/TEMP_MNT/EFI/BOOT
sudo grub-mkstandalone -O x86_64-efi -o "$BINARY_DIR"/TEMP_MNT/EFI/BOOT/BOOTX64.efi --modules="part_gpt multiboot2 fat" "boot/grub/grub.cfg=$BINARY_DIR/TEMP_MNT/boot/grub/grub.cfg"

sudo grub-install --target=i386-pc --install-modules="multiboot2" --root-directory="$BINARY_DIR"/TEMP_MNT --no-floppy --boot-directory="$BINARY_DIR"/TEMP_MNT/boot "$LOOP_DEVICE"

sudo umount "$BINARY_DIR"/TEMP_MNT
rm -rf "$BINARY_DIR"/TEMP_MNT
sudo losetup -d "$LOOP_DEVICE"
