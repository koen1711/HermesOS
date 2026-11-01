import filecmp
import json
import os
import re
import shutil
import subprocess
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

SECTOR = 512
MIB = 1 << 20
GIB = 1 << 30
TIB = 1 << 40

ESP_TYPE = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
BIOS_GRUB_TYPE = "21686148-6449-6E6F-744E-656564454649"
MBR_BOOT_CODE_BYTES = 440


class ImageError(Exception):
    pass


@dataclass(frozen=True)
class DiskConfig:
    binary_dir: Path
    default_size: int = 16 * GIB
    min_size: int = 64 * MIB
    max_size: int = 2 * TIB
    fat_serial: str = "48455253"
    fat_label: str = "EFI SYSTEM"
    bios_grub_start: int = 1 * MIB
    bios_grub_size: int = 2 * MIB
    tail_reserved: int = 1 * MIB

    @property
    def core_img(self) -> Path:
        return self.binary_dir / "core.img"

    @property
    def isodir(self) -> Path:
        return self.binary_dir / "isodir"

    @property
    def grub_cache(self) -> Path:
        return self.binary_dir / "grub-cache"

    @property
    def fat_start(self) -> int:
        return self.bios_grub_start + self.bios_grub_size


def parse_size(text: str, config: DiskConfig) -> int:
    match = re.fullmatch(r"\s*(\d+)\s*([MGT])(?:i?B)?\s*", text, re.IGNORECASE)
    if not match:
        raise ImageError(f"invalid size '{text.strip()}' (examples: 512M, 32G, 1T)")
    unit = {"M": MIB, "G": GIB, "T": TIB}[match.group(2).upper()]
    size = int(match.group(1)) * unit
    if not config.min_size <= size <= config.max_size:
        raise ImageError(
            f"size must be between {format_size(config.min_size)} and {format_size(config.max_size)}"
        )
    return size


def format_size(size: int) -> str:
    for unit, suffix in ((TIB, "T"), (GIB, "G"), (MIB, "M")):
        if size >= unit:
            value = size / unit
            return f"{value:.0f}{suffix}" if value == int(value) else f"{value:.1f}{suffix}"
    return f"{size}B"


def _run(*args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def _require(*tools):
    missing = [tool for tool in tools if not shutil.which(tool)]
    if missing:
        raise ImageError(f"missing tools: {', '.join(missing)} (install mtools, fdisk and grub)")


def _partition_table(config: DiskConfig, img: Path, size: int):
    fat_size = size - config.fat_start - config.tail_reserved
    return (
        "label: gpt\n"
        f'{img.name}1 : start={config.fat_start // SECTOR}, size={fat_size // SECTOR}, '
        f'type={ESP_TYPE}, name="primary"\n'
        f'{img.name}2 : start={config.bios_grub_start // SECTOR}, size={config.bios_grub_size // SECTOR}, '
        f'type={BIOS_GRUB_TYPE}, name="primary"\n'
    )


def _write_blank(config: DiskConfig, img: Path, size: int):
    img.unlink(missing_ok=True)
    os.truncate(_touch(img), size)
    _run("sfdisk", "-q", "--no-reread", "--no-tell-kernel", img,
         input=_partition_table(config, img, size), text=True)


def _touch(path: Path) -> Path:
    path.touch()
    return path


def _fat_partition(img: Path):
    table = json.loads(_run("sfdisk", "-J", img, capture_output=True, text=True).stdout)
    for partition in table["partitiontable"]["partitions"]:
        if partition["type"].upper() == ESP_TYPE:
            return partition["start"], partition["size"]
    raise ImageError(f"{img} has no FAT partition")


def _fat_spec(img: Path) -> str:
    start, _ = _fat_partition(img)
    return f"{img}@@{start * SECTOR}"


def _format_fat(config: DiskConfig, img: Path):
    start, sectors = _fat_partition(img)
    _run("mformat", "-i", f"{img}@@{start * SECTOR}", "-F", "-N", config.fat_serial,
         "-v", config.fat_label, "-T", sectors, "-h", 64, "-s", 32, "-H", start, "::")


def _copy_into(fat: str, sources, dest: str, preserve_times: bool = False):
    sources = list(sources)
    if not sources:
        return
    flags = ["-s", "-Q", "-D", "o"] + (["-m"] if preserve_times else [])
    _run("mcopy", "-i", fat, *flags, *sources, dest)


def _make_dirs(fat: str, *paths):
    for path in paths:
        subprocess.run(["mmd", "-i", fat, "-D", "s", path],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def _boot_cache_ready(config: DiskConfig) -> bool:
    cache = config.grub_cache
    return all(p.exists() for p in (cache / "boot.bin", cache / "bios_grub.bin", cache / "grub"))


def bootstrap(config: DiskConfig):
    _require("sudo", "losetup", "mkfs.vfat", "grub-install", "sfdisk")
    print("Installing GRUB once to cache its boot code (needs sudo)...", flush=True)
    cache = config.grub_cache
    shutil.rmtree(cache, ignore_errors=True)
    cache.mkdir(parents=True)

    with tempfile.TemporaryDirectory(dir=config.binary_dir, prefix="disk-mngr-boot-") as tmp:
        img = Path(tmp) / "boot.img"
        mnt = Path(tmp) / "mnt"
        mnt.mkdir()
        _write_blank(config, img, config.min_size)
        loop = _run("sudo", "losetup", "--find", "--show", "--partscan", img,
                    capture_output=True, text=True).stdout.strip()
        mounted = False
        try:
            part = f"{loop}p1"
            for _ in range(50):
                if os.path.exists(part):
                    break
                time.sleep(0.1)
            _run("sudo", "mkfs.vfat", "-F", 32, "-i", config.fat_serial, "-n", config.fat_label, part)
            _run("sudo", "mount", part, mnt)
            mounted = True
            _run("sudo", "grub-install", "--target=i386-pc", "--install-modules=multiboot2",
                 f"--root-directory={mnt}", "--no-floppy", f"--boot-directory={mnt}/boot", loop)
            shutil.copytree(mnt / "boot" / "grub", cache / "grub")
            _run("sudo", "umount", mnt)
            mounted = False
        finally:
            if mounted:
                subprocess.run(["sudo", "umount", str(mnt)])
            subprocess.run(["sudo", "losetup", "-d", loop])

        with img.open("rb") as source:
            (cache / "boot.bin").write_bytes(source.read(MBR_BOOT_CODE_BYTES))
            source.seek(config.bios_grub_start)
            (cache / "bios_grub.bin").write_bytes(source.read(config.bios_grub_size))


def _stamp_boot_code(config: DiskConfig, img: Path):
    with img.open("r+b") as target:
        target.write((config.grub_cache / "boot.bin").read_bytes())
        target.seek(config.bios_grub_start)
        target.write((config.grub_cache / "bios_grub.bin").read_bytes())


def _build_blank_image(config: DiskConfig, img: Path, size: int):
    _require("sfdisk", "mformat", "mcopy", "mmd")
    if not _boot_cache_ready(config):
        bootstrap(config)
    _write_blank(config, img, size)
    _stamp_boot_code(config, img)
    _format_fat(config, img)


def _efi_binary(config: DiskConfig) -> Path:
    _require("grub-mkstandalone")
    grub_cfg = config.isodir / "boot" / "grub" / "grub.cfg"
    efi_bin = config.grub_cache / "BOOTX64.efi"
    cached_cfg = config.grub_cache / "grub.cfg.efi"
    if not (efi_bin.exists() and cached_cfg.exists() and filecmp.cmp(grub_cfg, cached_cfg, shallow=False)):
        config.grub_cache.mkdir(parents=True, exist_ok=True)
        _run("grub-mkstandalone", "-O", "x86_64-efi", "-o", efi_bin,
             "--modules=part_gpt multiboot2 fat", f"boot/grub/grub.cfg={grub_cfg}")
        shutil.copyfile(grub_cfg, cached_cfg)
    return efi_bin


def _install_build_output(config: DiskConfig, img: Path):
    if not config.isodir.is_dir():
        raise ImageError(f"{config.isodir} not found, build the project first")
    fat = _fat_spec(img)
    _copy_into(fat, sorted(config.isodir.iterdir()), "::/")
    _make_dirs(fat, "::/boot", "::/boot/grub", "::/EFI", "::/EFI/BOOT")
    _run("mcopy", "-i", fat, "-Q", "-D", "o", _efi_binary(config), "::/EFI/BOOT/BOOTX64.efi")


def create(config: DiskConfig, size: int):
    print(f"Creating fresh {format_size(size)} image...", flush=True)
    img = config.core_img
    pending = img.with_name(img.name + ".new")
    try:
        _build_blank_image(config, pending, size)
        fat = _fat_spec(pending)
        _make_dirs(fat, "::/boot", "::/boot/grub")
        _copy_into(fat, sorted((config.grub_cache / "grub").iterdir()), "::/boot/grub/")
        _install_build_output(config, pending)
        os.replace(pending, img)
    finally:
        pending.unlink(missing_ok=True)


def refresh(config: DiskConfig):
    print("Refreshing build output on existing image...", flush=True)
    _require("sfdisk", "mcopy", "mmd")
    _install_build_output(config, config.core_img)


def resize(config: DiskConfig, size: int):
    img = config.core_img
    print(f"Resizing image to {format_size(size)}...", flush=True)
    _require("sfdisk", "mcopy")
    pending = img.with_name(img.name + ".new")
    with tempfile.TemporaryDirectory(dir=config.binary_dir, prefix="disk-mngr-resize-") as tmp:
        try:
            _run("mcopy", "-i", _fat_spec(img), "-s", "-Q", "-m", "::/*", tmp)
            used = sum(p.stat().st_size for p in Path(tmp).rglob("*") if p.is_file())
            capacity = size - config.fat_start - config.tail_reserved
            if used > capacity:
                raise ImageError(
                    f"image holds {format_size(used)} of files, too much for {format_size(size)}; "
                    "original left unchanged"
                )
            _build_blank_image(config, pending, size)
            try:
                _copy_into(_fat_spec(pending), sorted(Path(tmp).iterdir()), "::/", preserve_times=True)
            except subprocess.CalledProcessError as error:
                raise ImageError(
                    f"files don't fit in {format_size(size)}; original left unchanged"
                ) from error
            os.replace(pending, img)
        finally:
            pending.unlink(missing_ok=True)
