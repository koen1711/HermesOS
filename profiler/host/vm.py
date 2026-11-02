import os
import re
import shutil
import socket
import subprocess
import tempfile
import time

from .samples import Control, parse_control
from .session import CONTROL_SIZE, RUN_SCRIPT_HINT, ProfilerError, effective_rate

CONTROL_SYMBOL = "profiler_control"
WRITABLE_FIELDS = ("requested", "generation", "rate_hz", "selftest_spin_ms")
PROMPT = b"(qemu) "
MONITOR_CONNECT_TIMEOUT_S = 10
MONITOR_TIMEOUT_S = 120
GDB_TIMEOUT_S = 30
WRITE_ATTEMPTS = 3
RETRY_DELAY_S = 0.2
_ESCAPE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def symbol_address(elf: str, name: str) -> int:
    try:
        out = subprocess.run(["nm", elf], capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.SubprocessError) as e:
        raise ProfilerError(f"cannot read symbols of {elf}: {e}; {RUN_SCRIPT_HINT}") from None
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    raise ProfilerError(f"{elf} has no {name} symbol: it was not built with HERMES_PROFILER")


def _quote(path: str) -> str:
    if "\n" in path:
        raise ProfilerError(f"cannot pass a path with a newline to the monitor: {path!r}")
    return '"' + path.replace("\\", "\\\\").replace('"', '\\"') + '"'


class Vm:
    def __init__(self, monitor_socket: str, gdb_port: int, elf: str, symbol_addr: int):
        self.monitor_socket = monitor_socket
        self.gdb_port = gdb_port
        self.elf = elf
        self.symbol_addr = symbol_addr

    def alive(self) -> bool:
        with socket.socket(socket.AF_UNIX) as s:
            try:
                s.connect(self.monitor_socket)
            except OSError:
                return False
        return True

    def _read_prompt(self, s: socket.socket) -> str:
        data = b""
        while not data.endswith(PROMPT):
            chunk = s.recv(65536)
            if not chunk:
                raise ConnectionError("connection closed")
            data += chunk
        return _ESCAPE.sub("", data[:-len(PROMPT)].decode(errors="replace"))

    def _monitor(self, command: str) -> str:
        try:
            with socket.socket(socket.AF_UNIX) as s:
                s.settimeout(MONITOR_CONNECT_TIMEOUT_S)
                s.connect(self.monitor_socket)
                self._read_prompt(s)
                s.settimeout(MONITOR_TIMEOUT_S)
                s.sendall(command.encode() + b"\n")
                reply = self._read_prompt(s)
        except TimeoutError:
            raise ProfilerError(f"the QEMU monitor at {self.monitor_socket} did not answer: "
                                "is another client connected to it?") from None
        except OSError as e:
            raise ProfilerError(f"cannot reach the QEMU monitor at {self.monitor_socket}: {e}") from None
        return reply.split("\n", 1)[1].strip() if "\n" in reply else ""

    def read_control(self) -> Control:
        temp = tempfile.mkdtemp(prefix="hermes-profiler-")
        try:
            path = os.path.join(temp, "control.bin")
            self.save_phys(self.symbol_addr, CONTROL_SIZE, path)
            with open(path, "rb") as f:
                data = f.read()
        finally:
            shutil.rmtree(temp, ignore_errors=True)
        try:
            return parse_control(data)
        except ValueError as e:
            raise ProfilerError(f"cannot read the control struct ({e}): the guest may still be booting, "
                                f"or {self.elf} is not the running kernel") from None

    def save_phys(self, addr: int, size: int, path: str) -> None:
        path = os.path.abspath(path)
        reply = self._monitor(f"pmemsave {addr} {size} {_quote(path)}")
        try:
            saved = os.path.getsize(path)
        except OSError:
            saved = -1
        if saved != size:
            raise ProfilerError(f"pmemsave of {size} bytes at {addr:#x} to {path} wrote {saved}: {reply}")

    def read_phys(self, addr: int, size: int) -> bytes:
        temp = tempfile.mkdtemp(prefix="hermes-profiler-")
        try:
            path = os.path.join(temp, "memory.bin")
            self.save_phys(addr, size, path)
            with open(path, "rb") as f:
                return f.read()
        finally:
            shutil.rmtree(temp, ignore_errors=True)

    def _run_gdb(self, fields: dict[str, int]) -> str:
        args = ["gdb", "-q", "-nx", "-batch",
                "-ex", "set tcp auto-retry off",
                "-ex", f"file {_quote(self.elf)}",
                "-ex", f"target remote :{self.gdb_port}"]
        for field, value in fields.items():
            args += ["-ex", f"set var {CONTROL_SYMBOL}.{field}={value}"]
        args += ["-ex", "detach"]
        try:
            done = subprocess.run(args, capture_output=True, text=True, timeout=GDB_TIMEOUT_S,
                                  stdin=subprocess.DEVNULL)
        except subprocess.TimeoutExpired:
            return f"gdb timed out after {GDB_TIMEOUT_S}s"
        except OSError as e:
            raise ProfilerError(f"cannot run gdb: {e}") from None
        return done.stderr

    def _resume_if_paused(self) -> None:
        if "paused" in self._monitor("info status"):
            self._monitor("cont")

    def _gdb_then_resume(self, fields: dict[str, int]) -> str:
        try:
            return self._run_gdb(fields)
        finally:
            self._resume_if_paused()

    @staticmethod
    def _mismatch(control: Control, fields: dict[str, int]) -> str | None:
        for field, value in fields.items():
            actual = getattr(control, field)
            if actual != value and not (field == "rate_hz" and actual == effective_rate(value)):
                return f"{field} is {actual}, expected {value}"
        return None

    def set_control(self, **fields: int) -> None:
        unknown = [f for f in fields if f not in WRITABLE_FIELDS]
        if unknown:
            raise ProfilerError(f"cannot write control fields {unknown}")
        problem = None
        for attempt in range(WRITE_ATTEMPTS):
            if attempt:
                time.sleep(RETRY_DELAY_S)
            output = ""
            try:
                output = self._gdb_then_resume(fields)
                problem = self._mismatch(self.read_control(), fields)
            except ProfilerError as e:
                problem = str(e)
            if problem is None:
                return
            problem = f"{problem} ({output.strip()})" if output.strip() else problem
        raise ProfilerError(f"could not write the control struct after {WRITE_ATTEMPTS} attempts: {problem}")
