#!/bin/sh

# Read the BINARY_DIR as the first argument
BINARY_DIR=$1

# Check if BINARY_DIR/core.img already exists
if [ -f "$BINARY_DIR"/core.img ]; then
    rm "$BINARY_DIR"/core.img
fi

# Check if BINARY_DIR/base.img already exists
if  [ -f "$BINARY_DIR"/base.img ]; then
    cp "$BINARY_DIR"/base.img "$BINARY_DIR"/core.img
else
  dd if=/dev/zero of="$BINARY_DIR"/base.img bs=1M count=1024
  # Set up GPT partition table
  parted "$BINARY_DIR"/base.img --script -- \
          mklabel gpt \
          mkpart primary fat32 1MiB 513MiB \
          mkpart primary 513MiB 515MiB \
          set 1 boot on \
          set 1 esp on \
          set 2 bios_grub on
  cp "$BINARY_DIR"/base.img "$BINARY_DIR"/core.img
fi

# Format the partition as FAT32
LOOP_DEVICE=$(sudo losetup --find --show --partscan "$BINARY_DIR"/core.img)
sudo mkfs.vfat -n "EFI SYSTEM" "$LOOP_DEVICE"p1

mkdir -p "$BINARY_DIR"/TEMP_MNT
# Mount the partition
sudo mount "$LOOP_DEVICE"p1 "$BINARY_DIR"/TEMP_MNT

# Copy the entire BINARY_DIR/isodir to the mounted image
# Recursively loop through the BINARY_DIR/isodir and copy all files to the mounted image
sudo cp -r "$BINARY_DIR"/isodir/* "$BINARY_DIR"/TEMP_MNT/

# Generate GRUB image
sudo mkdir -p "$BINARY_DIR"/TEMP_MNT/EFI/BOOT
sudo grub-mkstandalone -O x86_64-efi -o "$BINARY_DIR"/TEMP_MNT/EFI/BOOT/BOOTX64.efi --modules="part_gpt multiboot2 fat" "boot/grub/grub.cfg=$BINARY_DIR/TEMP_MNT/boot/grub/grub.cfg"
# Also create legacy boot mode
sudo grub-install --target=i386-pc --install-modules="multiboot2" --root-directory="$BINARY_DIR"/TEMP_MNT --no-floppy --boot-directory="$BINARY_DIR"/TEMP_MNT/boot "$LOOP_DEVICE"

# Remove the temporary mount point
sudo umount "$BINARY_DIR"/TEMP_MNT
rm -rf "$BINARY_DIR"/TEMP_MNT
sudo losetup -d "$LOOP_DEVICE"
