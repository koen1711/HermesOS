#!/bin/sh
set -e
export PATH="/mingw64/bin:/usr/bin:$PATH"

BINARY_DIR=$1
if [ -z "$BINARY_DIR" ]; then
    echo "Usage: $0 <BINARY_DIR>" >&2
    exit 1
fi

IMG=$BINARY_DIR/core.img
IMG_WIN=$(cygpath -w "$IMG")

# FIX: Pointed directly to the deep directory where part_gpt.mod and other modules live
GRUB_MODULES_WINDOWS="C:\\Users\\manue\\grub-modules\\usr\\lib\\grub\\x86_64-efi"
GRUB_MODULES_DIR=$(cygpath -u "$GRUB_MODULES_WINDOWS")

for tool in mformat mcopy python3 grub-mkimage; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: '$tool' not found in MSYS2 PATH." >&2
        exit 1
    fi
done

ISODIR=$BINARY_DIR/isodir
if [ ! -d "$ISODIR" ]; then
    echo "ERROR: $ISODIR not found — build the OS target first." >&2
    exit 1
fi

# Fix Windows line endings in grub.cfg
sed -i 's/\r//' "$ISODIR/boot/grub/grub.cfg"

rm -f "$IMG"

echo "Creating 512 MiB raw image"
dd if=/dev/zero of="$IMG_WIN" bs=1M count=512 status=none

echo "Writing GPT"
python3 - "$IMG_WIN" <<'PYEOF'
import struct, sys, uuid, zlib

def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF

img = sys.argv[1]
total_sectors = 512 * 1024 * 2
part_start    = 2048
part_end      = total_sectors - 34

with open(img, 'r+b') as f:
    f.seek(446)
    f.write(bytes([0x00, 0x00, 0x02, 0x00, 0xEE, 0xFF, 0xFF, 0xFF,
                   0x01, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF]))
    f.seek(510)
    f.write(b'\x55\xAA')

    disk_guid  = uuid.uuid4().bytes_le
    part_guid  = uuid.uuid4().bytes_le
    type_guid  = bytes.fromhex('EBD0A0A2B9E5443387C068B6B72699C7')

    name_utf16  = 'EFI SYSTEM'.encode('utf-16-le').ljust(72, b'\x00')
    part_entry  = type_guid + part_guid + struct.pack('<QQQ', part_start, part_end, 0) + name_utf16
    part_entries = part_entry + bytes(128 * 127)
    parts_crc    = crc32(part_entries)

    def make_header(my_lba, alt_lba, parts_lba):
        hdr = struct.pack('<8sIIIIQQQQ16sQIII',
            b'EFI PART', 0x00010000, 92, 0, 0,
            my_lba, alt_lba, 34, total_sectors - 1,
            disk_guid, parts_lba, 128, 128, parts_crc)
        hdr_crc = crc32(hdr)
        return hdr[:16] + struct.pack('<I', hdr_crc) + hdr[20:]

    primary_hdr = make_header(1, total_sectors - 1, 2)
    backup_hdr  = make_header(total_sectors - 1, 1, total_sectors - 33)

    f.seek(512);  f.write(primary_hdr)
    f.seek(1024); f.write(part_entries)
    f.seek((total_sectors - 33) * 512); f.write(part_entries)
    f.seek((total_sectors - 1)  * 512); f.write(backup_hdr)

print("GPT written successfully")
PYEOF

PART_OFFSET=1048576

echo "Formatting partition 1 as FAT32"
mformat -i "$IMG_WIN@@$PART_OFFSET" -F -v "EFISYSTEM" ::

echo "Populating partition from $ISODIR"
mcopy -i "$IMG_WIN@@$PART_OFFSET" -s -Q -o "$ISODIR"/* ::/

echo "Generating GRUB EFI image"
rm -f /tmp/grub-efi-out/BOOTX64.efi
mkdir -p /tmp/grub-efi-out

# FIX: Explicitly embedding video and terminal modules directly into the EFI app
grub-mkimage \
    --directory="$GRUB_MODULES_WINDOWS" \
    --prefix="(hd0,gpt1)/boot/grub" \
    --output="/tmp/grub-efi-out/BOOTX64.efi" \
    --format="x86_64-efi" \
    part_gpt multiboot2 fat normal configfile efi_gop efi_uga all_video gfxterm font loadenv

echo "Installing EFI bootloader"
mmd -i "$IMG_WIN@@$PART_OFFSET" ::/EFI ::/EFI/BOOT 2>/dev/null || true
mcopy -i "$IMG_WIN@@$PART_OFFSET" -o /tmp/grub-efi-out/BOOTX64.efi ::/EFI/BOOT/

echo "Copying GRUB modules to image"
mmd -i "$IMG_WIN@@$PART_OFFSET" ::/boot/grub/x86_64-efi 2>/dev/null || true
mcopy -i "$IMG_WIN@@$PART_OFFSET" -o "$GRUB_MODULES_DIR"/*.mod ::/boot/grub/x86_64-efi/

echo "Wrote $IMG"