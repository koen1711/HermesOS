import struct
from dataclasses import dataclass
from typing import Iterator

MAGIC = 0x4C5443464F525048
VERSION = 1
MAX_DEPTH = 32
MAX_CPUS = 16

FLAG_USER = 1
FLAG_TRUNCATED = 2
FLAG_INTERRUPTS_ON = 4

_RECORD = struct.Struct("<QQHBBIQ")
_HEADER = struct.Struct("<8Q")
_CPU = struct.Struct("<14Q")
_CPU_FIELDS = ("status", "armed_generation", "nmi_count", "samples", "dropped", "truncated",
               "user_samples", "write_offset", "buffer_phys", "buffer_size", "handler_tsc_total",
               "start_tsc", "stop_tsc", "spurious")


@dataclass
class Sample:
    tsc: int
    rip: int
    cpu: int
    user: bool
    truncated: bool
    frames: list[int]  # return addresses above rip, leaf first
    interrupts_on: bool = False


@dataclass
class CpuState:
    status: int
    armed_generation: int
    nmi_count: int
    samples: int
    dropped: int
    truncated: int
    user_samples: int
    write_offset: int
    buffer_phys: int
    buffer_size: int
    handler_tsc_total: int
    start_tsc: int
    stop_tsc: int
    spurious: int


@dataclass
class Control:
    version: int
    requested: int
    generation: int
    rate_hz: int
    selftest_spin_ms: int
    ncpus: int
    tsc_hz: int
    cpu: list[CpuState]


def parse_control(data: bytes) -> Control:
    size = _HEADER.size + MAX_CPUS * _CPU.size
    if len(data) < size:
        raise ValueError(f"control struct is {len(data)} bytes, expected {size}")
    magic, version, requested, generation, rate_hz, spin_ms, ncpus, tsc_hz = _HEADER.unpack_from(data, 0)
    if magic != MAGIC:
        raise ValueError(f"bad control magic {magic:#x}")
    if version != VERSION:
        raise ValueError(f"unsupported control version {version}")
    cpus = [CpuState(*_CPU.unpack_from(data, _HEADER.size + i * _CPU.size)) for i in range(MAX_CPUS)]
    return Control(version, requested, generation, rate_hz, spin_ms, ncpus, tsc_hz, cpus)


def parse_samples(data: bytes) -> Iterator[Sample]:
    offset = 0
    while offset < len(data):
        if len(data) - offset < _RECORD.size:
            raise ValueError(f"truncated record header at offset {offset}")
        tsc, rip, depth, cpu, flags, _, _ = _RECORD.unpack_from(data, offset)
        if depth > MAX_DEPTH:
            raise ValueError(f"depth {depth} exceeds {MAX_DEPTH} at offset {offset}")
        end = offset + _RECORD.size + depth * 8
        if end > len(data):
            raise ValueError(f"truncated record frames at offset {offset}")
        frames = list(struct.unpack_from(f"<{depth}Q", data, offset + _RECORD.size))
        yield Sample(tsc, rip, cpu, bool(flags & FLAG_USER), bool(flags & FLAG_TRUNCATED), frames,
                     bool(flags & FLAG_INTERRUPTS_ON))
        offset = end
