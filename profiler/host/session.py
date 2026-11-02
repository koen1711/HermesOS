import json
import os
import shutil
import struct
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import Iterator

from .aggregate import build_report_data
from .errors import ProfilerError
from .report import render_report, top_functions
from .samples import MAX_CPUS, Control, CpuState, Sample, parse_control, parse_samples
from .symbols import SymbolTable, executable_image

OFF, ARMED, NO_PMU, NO_BUFFER = range(4)
STATUS_NAMES = {OFF: "off", ARMED: "armed", NO_PMU: "no-pmu", NO_BUFFER: "no-buffer"}
CONTROL_SIZE = 64 + MAX_CPUS * 112
DEFAULT_RATE_HZ = 1000
MAX_RATE_HZ = 10000
POLL_S = 0.05
TOP_N = 20
RUN_SCRIPT_HINT = "boot the guest with run-profiler.sh"
ELF_NAME = "kernel.elf"
SESSION_FIELDS = ("monitor_socket", "gdb_port", "elf", "output_root", "qemu_pidfile", "kernel_commit", "kernel_dirty",
                  "build_flags", "recording", "started_at")


def default_session_path() -> str:
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    if runtime:
        return os.path.join(runtime, "hermes-profiler", "session.json")
    return f"/tmp/hermes-profiler-{os.getuid()}/session.json"


SESSION_PATH = default_session_path()


def effective_rate(rate_hz: int) -> int:
    if rate_hz == 0:
        return DEFAULT_RATE_HZ
    return min(rate_hz, MAX_RATE_HZ)


def load_session() -> dict:
    try:
        with open(SESSION_PATH, encoding="utf-8") as f:
            session = json.load(f)
    except FileNotFoundError:
        raise ProfilerError(f"no profiler session: {RUN_SCRIPT_HINT}") from None
    except (OSError, ValueError) as e:
        raise ProfilerError(f"cannot read {SESSION_PATH}: {e}; {RUN_SCRIPT_HINT}") from None
    if not isinstance(session, dict) or any(field not in session for field in SESSION_FIELDS):
        raise ProfilerError(f"{SESSION_PATH} is incomplete: {RUN_SCRIPT_HINT}")
    return session


def save_session(session: dict) -> None:
    path = Path(SESSION_PATH)
    temp = path.with_name(f"{path.name}.{os.getpid()}")
    try:
        path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        temp.write_text(json.dumps(session, indent=2) + "\n", encoding="utf-8")
        os.replace(temp, path)
    except OSError as e:
        _remove_quietly(temp)
        raise ProfilerError(f"cannot save {path}: {e}") from None


def _remove_quietly(path: Path) -> None:
    try:
        path.unlink(missing_ok=True)
    except OSError:
        pass


def begin_session(monitor_socket: str, gdb_port: int, elf: str, output_root: str, qemu_pidfile: str,
                  kernel_commit: str, kernel_dirty: bool | None, build_flags: str) -> None:
    try:
        previous = load_session()
    except ProfilerError:
        previous = None
    pinned = Path(SESSION_PATH).with_name(ELF_NAME)
    temp = pinned.with_name(f"{ELF_NAME}.{os.getpid()}")
    try:
        pinned.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        shutil.copyfile(elf, temp)
        os.replace(temp, pinned)
    except OSError as e:
        _remove_quietly(temp)
        raise ProfilerError(f"cannot pin the kernel ELF for this session: {e}") from None
    save_session({
        "monitor_socket": monitor_socket,
        "gdb_port": gdb_port,
        "elf": str(pinned),
        "output_root": output_root,
        "qemu_pidfile": qemu_pidfile,
        "kernel_commit": kernel_commit,
        "kernel_dirty": kernel_dirty,
        "build_flags": build_flags,
        "recording": False,
        "started_at": None,
    })
    if previous and previous.get("recording"):
        print("note: the previous session was still recording when it ended: that round's samples were lost")


def session_dir() -> str:
    path = Path(SESSION_PATH).parent
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    return str(path)


def _process_name(pid: int) -> str | None:
    try:
        return Path(f"/proc/{pid}/comm").read_text().strip()
    except OSError:
        return None


def live_guest_pid(session: dict) -> int | None:
    try:
        pid = int(Path(session["qemu_pidfile"]).read_text().strip())
        os.kill(pid, 0)
    except PermissionError:
        pass
    except (OSError, ValueError, KeyError):
        return None
    name = _process_name(pid)
    return pid if name is None or name.startswith("qemu") else None


def refuse_live_session() -> None:
    try:
        session = load_session()
    except ProfilerError:
        return
    pid = live_guest_pid(session)
    if pid is not None:
        raise ProfilerError(f"a profiled guest is already running (QEMU pid {pid}): stop it first, because a new "
                            f"session would replace its pinned kernel ELF and break its symbols")


def end_session() -> None:
    if load_session().get("recording"):
        return
    try:
        Path(SESSION_PATH).with_name(ELF_NAME).unlink(missing_ok=True)
        os.unlink(SESSION_PATH)
    except OSError as e:
        raise ProfilerError(f"cannot remove {SESSION_PATH}: {e}") from None


def _qemu_pid_alive(session: dict) -> bool:
    try:
        pid = int(Path(session["qemu_pidfile"]).read_text().strip())
        os.kill(pid, 0)
    except PermissionError:
        return True
    except (OSError, ValueError, KeyError):
        return False
    return True


def _guest_running(vm, session: dict) -> bool:
    return _qemu_pid_alive(session) and vm.alive()


def _cpus(control: Control) -> list[CpuState]:
    return control.cpu[:control.ncpus]


def _with_status(control: Control, status: int) -> list[int]:
    return [i for i, cpu in enumerate(_cpus(control)) if cpu.status == status]


def _names(cpus: list[int]) -> str:
    return "cpu " + ", ".join(str(i) for i in cpus)


def _wait(vm, timeout_s: float, done) -> tuple[Control, bool]:
    deadline = time.monotonic() + timeout_s
    while True:
        control = vm.read_control()
        if done(control):
            return control, True
        if time.monotonic() >= deadline:
            return control, False
        time.sleep(POLL_S)


def _abort_start(vm, timeout_s: float) -> str:
    try:
        vm.set_control(requested=0)
        control, done = _wait(vm, timeout_s, lambda c: not _with_status(c, ARMED))
    except ProfilerError as e:
        return f"; could not disarm, so the kernel may still be recording (try `profiler stop`): {e}"
    except KeyboardInterrupt:
        return "; disarming was interrupted, so the kernel may still be recording (try `profiler stop`)"
    if not done:
        return (f"; {_names(_with_status(control, ARMED))} did not disarm, "
                "so the kernel may still be recording (try `profiler stop`)")
    return ""


def check_kernel(vm, session: dict) -> None:
    for base, expected in executable_image(session["elf"]):
        actual = vm.read_phys(base, len(expected))
        if actual != expected and not any(actual):
            raise ProfilerError("the guest has not loaded a kernel yet: its memory where the kernel code belongs is "
                                "blank, so it is still booting or did not boot from this image")
        if actual != expected:
            offset = next((i for i, (a, b) in enumerate(zip(actual, expected)) if a != b), min(len(actual), len(expected)))
            raise ProfilerError(
                f"the guest is running an older kernel than the one pinned for this session: its code differs from "
                f"{session['elf']} at {base + offset:#x}. The disk image was not rebuilt (DISK_MODE=keep boots it as it "
                f"is): rebuild the image with run-profiler.sh using DISK_MODE=continue or fresh")


def start(vm, rate_hz: int = DEFAULT_RATE_HZ, timeout_s: float = 5) -> None:
    session = load_session()
    if not _guest_running(vm, session):
        raise ProfilerError(f"QEMU is not running: {RUN_SCRIPT_HINT}")
    if rate_hz < 0:
        raise ProfilerError(f"invalid rate {rate_hz}: it must not be negative")
    check_kernel(vm, session)

    control = vm.read_control()
    if control.requested or (session.get("recording") and control.generation):
        raise ProfilerError("already recording: run `profiler stop` first")
    if session.get("recording"):
        print("note: the guest rebooted during the previous session: that round's samples were lost")
        session["recording"] = False
        save_session(session)
    no_buffer = _with_status(control, NO_BUFFER)
    if no_buffer:
        raise ProfilerError(f"{_names(no_buffer)} has no sample buffer: "
                            "the guest ran out of memory at boot, so give it more RAM or lower PROFILER_BUFFER_MIB")

    rate = effective_rate(rate_hz)
    if rate != rate_hz:
        print(f"note: the kernel accepts {rate} Hz for a request of {rate_hz} Hz")
    generation = control.generation + 1
    try:
        control = _arm(vm, rate, generation, timeout_s)
        session["recording"] = True
        session["started_at"] = datetime.now().isoformat(timespec="seconds")
        save_session(session)
    except ProfilerError as e:
        raise ProfilerError(f"{e}{_abort_start(vm, timeout_s)}") from None
    except KeyboardInterrupt:
        problem = _abort_start(vm, timeout_s)
        print(f"profiler: interrupted{problem or ', recording disarmed'}", file=sys.stderr)
        raise
    print(f"recording at {control.rate_hz} Hz on {control.ncpus} CPUs")


def _arm(vm, rate: int, generation: int, timeout_s: float) -> Control:
    vm.set_control(rate_hz=rate, generation=generation, requested=1)

    def settled(c: Control) -> bool:
        return all(cpu.armed_generation == generation and cpu.status != OFF for cpu in _cpus(c))

    control, done = _wait(vm, timeout_s, settled)
    no_pmu = _with_status(control, NO_PMU)
    if no_pmu:
        raise ProfilerError(f"{_names(no_pmu)} has no usable performance counter: "
                            "check that KVM's PMU is on (-enable-kvm -cpu host, kvm enable_pmu=1)")
    if not done:
        late = [i for i, cpu in enumerate(_cpus(control))
                if cpu.armed_generation != generation or cpu.status != ARMED]
        raise ProfilerError(f"{_names(late)} did not arm within {timeout_s:g}s: the guest may still be booting")
    return control


def _lost(session: dict, reason: str) -> ProfilerError:
    session["recording"] = False
    note = ""
    try:
        save_session(session)
    except ProfilerError as e:
        note = f" ({e})"
    return ProfilerError(f"{reason}: the samples of this session are lost{note}")


def stop(vm, session: dict, timeout_s: float = 5) -> str:
    recording = bool(session.get("recording"))
    if not _guest_running(vm, session):
        if recording:
            raise _lost(session, "QEMU is gone")
        raise ProfilerError("not recording: QEMU is not running either")
    try:
        directory = _stop_and_copy(vm, session, recording, timeout_s)
    except ProfilerError:
        if recording and not _guest_running(vm, session):
            raise _lost(session, "QEMU is gone") from None
        raise
    session["recording"] = False
    try:
        save_session(session)
    except ProfilerError as e:
        print(f"WARNING: {e}; the session may still look like it is recording")
    try:
        return report(directory)
    except ProfilerError as e:
        raise ProfilerError(f"samples saved in {directory}, but the report failed: {e}\n"
                            f"retry with: profiler report {directory}") from None


def _stop_and_copy(vm, session: dict, recording: bool, timeout_s: float) -> str:
    control = vm.read_control()
    if not recording and not control.requested:
        raise ProfilerError("not recording: run `profiler start` first")
    if control.generation == 0:
        raise _lost(session, "the guest rebooted during the session")

    vm.set_control(requested=0)
    control, done = _wait(vm, timeout_s, lambda c: not _with_status(c, ARMED))
    if not done:
        raise ProfilerError(f"{_names(_with_status(control, ARMED))} did not acknowledge stop within {timeout_s:g}s: "
                            "no report written, run `profiler stop` again to retry")
    return _copy_round(vm, session)


def _recorded_this_round(control: Control, cpu: CpuState) -> bool:
    return cpu.status == OFF and cpu.armed_generation == control.generation


def _copy_round(vm, session: dict) -> str:
    root = Path(session["output_root"])
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    temp = root / f".{stamp}.{os.getpid()}.partial"
    try:
        temp.mkdir(parents=True)
        vm.save_phys(vm.symbol_addr, CONTROL_SIZE, str(temp / "control.bin"))
        control = parse_control((temp / "control.bin").read_bytes())
        for i, cpu in enumerate(_cpus(control)):
            path = temp / f"samples.cpu{i}.bin"
            if cpu.status == ARMED:
                raise ProfilerError(f"cpu {i} was armed again during the copy")
            if cpu.write_offset > cpu.buffer_size:
                raise ProfilerError(f"cpu {i} reports {cpu.write_offset} bytes in a {cpu.buffer_size} byte buffer")
            if _recorded_this_round(control, cpu) and cpu.write_offset:
                vm.save_phys(cpu.buffer_phys, cpu.write_offset, str(path))
            else:
                path.write_bytes(b"")
        shutil.copyfile(session["elf"], temp / ELF_NAME)
        meta = _build_meta(control, session)
        (temp / "meta.json").write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
        return str(_publish(temp, root, stamp))
    except (OSError, ValueError) as e:
        raise ProfilerError(f"cannot write the session to {root}: {e}") from None
    finally:
        shutil.rmtree(temp, ignore_errors=True)


def _publish(temp: Path, root: Path, stamp: str) -> Path:
    attempt = 1
    while True:
        target = root / (stamp if attempt == 1 else f"{stamp}-{attempt}")
        if not target.exists():
            temp.rename(target)
            return target
        attempt += 1


def _build_meta(control: Control, session: dict) -> dict:
    cpus = [c for c in _cpus(control) if _recorded_this_round(control, c)]
    spans = [(c.start_tsc, c.stop_tsc) for c in cpus if c.stop_tsc > c.start_tsc]
    duration = (max(s[1] for s in spans) - min(s[0] for s in spans)) / control.tsc_hz if spans and control.tsc_hz else 0
    return {
        "kernel_commit": session.get("kernel_commit"),
        "kernel_dirty": session.get("kernel_dirty"),
        "build_flags": session.get("build_flags"),
        "cpu_count": control.ncpus,
        "rate_hz": control.rate_hz,
        "duration_s": duration,
        "tsc_hz": control.tsc_hz,
        "handler_tsc_total": sum(c.handler_tsc_total for c in cpus),
        "cpu_time_tsc": sum(stop - begin for begin, stop in spans),
        "spurious": sum(c.spurious for c in cpus),
        "dropped": sum(c.dropped for c in cpus),
        "missing_cpus": [i for i, c in enumerate(_cpus(control)) if not _recorded_this_round(control, c)],
    }


def _read_samples(directory: Path, control: Control) -> Iterator[Sample]:
    for i, cpu in enumerate(_cpus(control)):
        name = f"samples.cpu{i}.bin"
        data = (directory / name).read_bytes()
        expected_bytes, expected = (cpu.write_offset, cpu.samples) if _recorded_this_round(control, cpu) else (0, 0)
        if len(data) != expected_bytes:
            raise ProfilerError(f"{name} has {len(data)} bytes, control.bin says {expected_bytes}")
        parsed = 0
        for sample in parse_samples(data):
            parsed += 1
            yield sample
        if parsed != expected:
            raise ProfilerError(f"{name} has {parsed} samples, control.bin says {expected}")


def report(directory: str) -> str:
    d = Path(directory)
    out = d / "report.html"
    try:
        control = parse_control((d / "control.bin").read_bytes())
        meta = json.loads((d / "meta.json").read_text(encoding="utf-8"))
        samples = _read_samples(d, control)
        data = build_report_data(samples, SymbolTable(str(d / ELF_NAME)), meta)
        render_report(data, str(out))
    except (ProfilerError, OSError, ValueError, RuntimeError, IndexError, struct.error,
            subprocess.SubprocessError) as e:
        raise ProfilerError(f"cannot generate a report from {d}: {e}") from None
    _print_summary(str(out), data)
    return str(out)


def _print_summary(path: str, data: dict) -> None:
    summary = data["summary"]
    total = summary["samples"]
    print(f"report: {path}")
    print(f"{total} samples")
    if summary["dropped"]:
        print(f"WARNING: {summary['dropped']} samples were dropped because a buffer filled; this profile is incomplete")
    missing = data["meta"].get("missing_cpus")
    if missing:
        print(f"WARNING: {_names(missing)} recorded nothing this round: samples are missing from this profile")
    rows = top_functions(data, TOP_N)
    if rows:
        print(f"{'self':>10} {'self%':>6} {'total%':>6}  function")
    for name, self_count, total_count in rows:
        print(f"{self_count:>10} {100 * self_count / total:>5.1f}% {100 * total_count / total:>5.1f}%  {name}")


def status(vm) -> str:
    if not vm.alive():
        raise ProfilerError(f"QEMU is not running: {RUN_SCRIPT_HINT}")
    control = vm.read_control()
    state = "yes" if control.requested else "no"
    lines = [f"recording: {state}  rate: {control.rate_hz} Hz  generation: {control.generation}",
             f"{'cpu':>3} {'status':<9} {'samples':>10} {'dropped':>10} {'used MiB':>9} {'size MiB':>9} {'full':>6}"]
    warnings = []
    for i, cpu in enumerate(_cpus(control)):
        name = STATUS_NAMES.get(cpu.status, str(cpu.status))
        full = 100 * cpu.write_offset / cpu.buffer_size if cpu.buffer_size else 0
        lines.append(f"{i:>3} {name:<9} {cpu.samples:>10} {cpu.dropped:>10} {cpu.write_offset / 2 ** 20:>9.1f} "
                     f"{cpu.buffer_size / 2 ** 20:>9.1f} {full:>5.1f}%")
        if cpu.dropped:
            warnings.append(f"WARNING: cpu {i} is dropping samples, buffer full: {cpu.dropped} lost so far")
    return "\n".join(lines + warnings)
