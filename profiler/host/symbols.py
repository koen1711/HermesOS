import bisect
import re
import struct
import subprocess
from dataclasses import dataclass
from typing import Iterable

from .errors import ProfilerError

_SHF_ALLOC = 0x2
_SHF_EXECINSTR = 0x4
ADDR2LINE_TIMEOUT_S = 600


@dataclass(frozen=True)
class Frame:
    function: str
    file: str | None
    line: int | None
    inlined: bool


def _unknown(addr: int) -> list[Frame]:
    return [Frame(f"[unknown {addr:#x}]", None, None, False)]


_SHT_NOBITS = 8


def _executable_sections(elf_path: str) -> list[tuple[int, int, int, int]]:
    with open(elf_path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF":
        raise ValueError(f"{elf_path} is not an ELF file")
    if data[4] == 2:
        shoff = struct.unpack_from("<Q", data, 0x28)[0]
        shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
        section = struct.Struct("<IIQQQQ")
    else:
        shoff = struct.unpack_from("<I", data, 0x20)[0]
        shentsize, shnum = struct.unpack_from("<HH", data, 0x2E)
        section = struct.Struct("<IIIIII")
    sections = []
    for i in range(shnum):
        _, kind, flags, addr, offset, size = section.unpack_from(data, shoff + i * shentsize)
        if flags & _SHF_ALLOC and flags & _SHF_EXECINSTR and size:
            sections.append((addr, offset, size, kind))
    return sections


def _executable_ranges(elf_path: str) -> list[tuple[int, int]]:
    return [(addr, addr + size) for addr, _, size, _ in _executable_sections(elf_path)]


def executable_image(elf_path: str) -> list[tuple[int, bytes]]:
    with open(elf_path, "rb") as f:
        data = f.read()
    return [(addr, data[offset:offset + size]) for addr, offset, size, kind in _executable_sections(elf_path)
            if kind != _SHT_NOBITS]


class SymbolTable:
    def __init__(self, elf_path: str):
        self.elf_path = elf_path
        self._ranges = _executable_ranges(elf_path)
        self._cache: dict[int, list[Frame]] = {}
        self._nm: tuple[list[int], list[str]] | None = None

    def is_code(self, addr: int) -> bool:
        return any(lo <= addr < hi for lo, hi in self._ranges)

    def resolve(self, addrs: Iterable[int]) -> dict[int, list[Frame]]:
        wanted = list(dict.fromkeys(addrs))
        missing = [a for a in wanted if a not in self._cache]
        if missing:
            self._run_addr2line(missing)
        return {a: self._cache[a] for a in wanted}

    def _run_addr2line(self, addrs: list[int]) -> None:
        stdin = "".join(f"{a:#x}\n" for a in addrs)
        try:
            done = subprocess.run(["addr2line", "-a", "-f", "-i", "-e", self.elf_path], input=stdin,
                                  capture_output=True, text=True, check=False, timeout=ADDR2LINE_TIMEOUT_S)
        except subprocess.TimeoutExpired:
            raise ProfilerError(f"addr2line on {self.elf_path} timed out after {ADDR2LINE_TIMEOUT_S:g}s") from None
        if done.returncode:
            raise ProfilerError(f"addr2line on {self.elf_path} failed with status {done.returncode}: "
                                f"{done.stderr.strip()}")
        out = done.stdout
        entries: list[list[tuple[str, str]]] = []
        lines = out.splitlines()
        i = 0
        while i < len(lines):
            if lines[i].startswith("0x"):
                entries.append([])
                i += 1
                continue
            if not entries:
                raise ProfilerError(f"addr2line printed {lines[i]!r} before any address")
            entries[-1].append((lines[i], lines[i + 1] if i + 1 < len(lines) else "??:0"))
            i += 2
        if len(entries) != len(addrs):
            raise ProfilerError(f"addr2line returned {len(entries)} results for {len(addrs)} addresses")
        for addr, pairs in zip(addrs, entries):
            self._cache[addr] = self._frames(addr, pairs)

    def _frames(self, addr: int, pairs: list[tuple[str, str]]) -> list[Frame]:
        frames = []
        last = len(pairs) - 1
        for i, (name, loc) in enumerate(pairs):
            file, line = self._location(loc)
            if name == "??":
                name = self._nm_name(addr) if i == last else None
                if name is None:
                    continue
            frames.append(Frame(name, file, line, i < last))
        return frames or _unknown(addr)

    @staticmethod
    def _location(loc: str) -> tuple[str | None, int | None]:
        m = re.match(r"(.*):(\d+|\?)(?: \(discriminator \d+\))?$", loc)
        if not m or m.group(1) == "??":
            return None, None
        line = int(m.group(2)) if m.group(2).isdigit() and int(m.group(2)) else None
        return m.group(1), line

    def address_of(self, name: str) -> int | None:
        starts, names = self._nm_table()
        return starts[names.index(name)] if name in names else None

    def _nm_table(self) -> tuple[list[int], list[str]]:
        if self._nm is None:
            out = subprocess.run(["nm", "-n", self.elf_path], capture_output=True, text=True).stdout
            starts, names = [], []
            for line in out.splitlines():
                parts = line.split()
                if len(parts) == 3 and parts[1] in "TtWw":
                    starts.append(int(parts[0], 16))
                    names.append(parts[2])
            self._nm = (starts, names)
        return self._nm

    def _nm_name(self, addr: int) -> str | None:
        starts, names = self._nm_table()
        idx = bisect.bisect_right(starts, addr) - 1
        return names[idx] if idx >= 0 and self.is_code(addr) else None
