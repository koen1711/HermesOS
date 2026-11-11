#!/bin/sh
#
# Boot a HermesOS guest with the kernel sampling profiler, then, from another terminal:
#
#   profiler/profiler start [--rate HZ]   begin recording (refuses a guest whose kernel is not the one just built)
#   ... run the workload in the guest (e.g. the FastAPI load against 127.0.0.1:8000) ...
#   profiler/profiler stop                copy the samples out and write profiler/output/<stamp>/report.html
#   profiler/profiler status              per-CPU state and buffer fullness
#
# Settings: HERMES_PROGRAMS, PROFILER_BUILD_DIR, PROFILER_OUTPUT, PROFILER_BUFFER_MIB, SMP (at least 2),
# DISK_MODE (continue, the default when a disk exists, or fresh put this build on the disk; keep boots it as it is), DISK_SIZE.
# The first run copies $HERMES_PROGRAMS/build/image/core.img (with its installed Python packages) to the profiling image.
# QEMU's monitor socket and pidfile live in the per-user session directory, so QEMU_EXTRA_ARGS may be word-split safely.

set -e

CUR_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_DIR=$(cd "$CUR_DIR/.." && pwd)

HERMES_PROGRAMS=${HERMES_PROGRAMS:-/media/koen/HermesOS/os-measurements/hermes-programs}
PROFILER_BUILD_DIR=${PROFILER_BUILD_DIR:-$REPO_DIR/build/profiler}
PROFILER_OUTPUT=${PROFILER_OUTPUT:-$REPO_DIR/profiler/output}
PROFILER_BUFFER_MIB=${PROFILER_BUFFER_MIB:-48}

SYSROOT=$REPO_DIR/buildtools/cross
KERNEL_BUILD=${PROFILER_KERNEL_BUILD:-$PROFILER_BUILD_DIR/kernel}
IMAGE_DIR=$PROFILER_BUILD_DIR/image
PROGRAMS_ROOT=$HERMES_PROGRAMS/build/root

if [ ! -w /dev/kvm ]
then
	echo "/dev/kvm is not writable: the profiler needs KVM and -cpu host for the guest's performance counters"
	exit 1
fi

if [ ! -d "$PROGRAMS_ROOT" ]
then
	echo "$PROGRAMS_ROOT not found, build some programs first (e.g. tools/build-cpython.sh in $HERMES_PROGRAMS)"
	exit 1
fi

PYTHON_PROFILER="$CUR_DIR/profiler"

if [ -z "$SMP" ]
then
	HOST_CPUS=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
	SMP=$((HOST_CPUS / 2))
	[ "$SMP" -gt 16 ] && SMP=16
	if [ "$SMP" -lt 2 ]
	then
		if [ "$HOST_CPUS" -lt 2 ]
		then
			echo "the guest needs at least 2 CPUs and this host has $HOST_CPUS"
			exit 1
		fi
		echo "raising SMP from $SMP to 2: the kernel needs at least 2 CPUs to start its local APIC"
		SMP=2
	fi
fi
case "$SMP" in
	''|*[!0-9]*)
		echo "SMP must be a number, got '$SMP'"
		exit 1
		;;
esac
if [ "$SMP" -lt 2 ]
then
	echo "SMP=$SMP is not supported: the kernel needs at least 2 CPUs to initialise its local APIC (use SMP=2 or more)"
	exit 1
fi
export SMP

python3 "$PYTHON_PROFILER" _refuse-live-session

for PORT in 1234 8000
do
	if python3 -c 'import socket, sys; s = socket.socket(); s.settimeout(1); sys.exit(0 if s.connect_ex(("127.0.0.1", int(sys.argv[1]))) == 0 else 1)' "$PORT"
	then
		echo "port $PORT is already in use on this host (QEMU needs 1234 for the debug stub and 8000 for the guest forward): stop whatever listens on it"
		exit 1
	fi
done

SESSION_DIR=$(python3 "$PYTHON_PROFILER" _session-dir)
case "$SESSION_DIR" in
	*[[:space:]]*)
		echo "the session directory '$SESSION_DIR' contains whitespace, which QEMU_EXTRA_ARGS cannot carry: set XDG_RUNTIME_DIR to a path without spaces"
		exit 1
		;;
esac

SEED_IMAGE=$HERMES_PROGRAMS/build/image/core.img
mkdir -p "$IMAGE_DIR"
if [ ! -f "$IMAGE_DIR/core.img" ] && [ -f "$SEED_IMAGE" ]
then
	rm -f "$IMAGE_DIR/core.img.seeding"
	NEED_KB=$(du -sk "$SEED_IMAGE" | cut -f1)
	FREE_KB=$(df -Pk "$IMAGE_DIR" | awk 'NR==2 {print $4}')
	echo "seeding $IMAGE_DIR/core.img from $SEED_IMAGE ($((NEED_KB / 1024)) MiB on disk, sparse copy): this copy happens once and keeps the packages installed on that disk"
	if [ "$FREE_KB" -lt $((NEED_KB + 1048576)) ]
	then
		echo "not enough free space for the disk image: $((NEED_KB / 1024)) MiB needed plus 1024 MiB spare, $((FREE_KB / 1024)) MiB free where $IMAGE_DIR is; free some space or set PROFILER_BUILD_DIR elsewhere"
		exit 1
	fi
	if ! cp --sparse=always --reflink=auto "$SEED_IMAGE" "$IMAGE_DIR/core.img.seeding"
	then
		rm -f "$IMAGE_DIR/core.img.seeding"
		echo "copying $SEED_IMAGE failed"
		exit 1
	fi
	mv "$IMAGE_DIR/core.img.seeding" "$IMAGE_DIR/core.img"
fi
if [ -z "$DISK_MODE" ] && [ -f "$IMAGE_DIR/core.img" ]
then
	DISK_MODE=continue
fi
if [ -n "$DISK_MODE" ]
then
	export DISK_MODE
fi

cmake -S "$REPO_DIR" -B "$KERNEL_BUILD" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$REPO_DIR/buildtools/toolchain.cmake" \
	-DCMAKE_BUILD_TYPE=Release \
	-DHERMES_PROFILER=ON \
	-DHERMES_PROFILER_BUFFER_MIB="$PROFILER_BUFFER_MIB"
cmake --build "$KERNEL_BUILD"

rm -rf "$IMAGE_DIR/isodir"
mkdir -p "$IMAGE_DIR" "$PROFILER_OUTPUT"
cp -r "$KERNEL_BUILD/isodir" "$IMAGE_DIR/isodir"

if [ -x "$PROGRAMS_ROOT/usr/bin/gcc" ]
then
	set -- --exclude='./usr/lib/libpython3*.a'
else
	set -- --exclude='./usr/include' --exclude='*.a'
fi

tar -C "$PROGRAMS_ROOT" -chf - \
	--exclude='./usr/share/man' \
	--exclude='./usr/share/info' \
	--exclude='./usr/lib/pkgconfig' \
	--exclude='./usr/lib/python3.*/config-*' \
	"$@" \
	. | tar -C "$IMAGE_DIR/isodir" -xf -

cp -r "$HERMES_PROGRAMS/rootfs/." "$IMAGE_DIR/isodir/"

mkdir -p "$IMAGE_DIR/isodir/tmp"

mkdir -p "$IMAGE_DIR/isodir/etc/ssl/certs"
for bundle in /etc/ssl/certs/ca-certificates.crt /etc/pki/tls/certs/ca-bundle.crt /etc/ssl/cert.pem
do
	if [ -f "$bundle" ]
	then
		cp -L "$bundle" "$IMAGE_DIR/isodir/etc/ssl/cert.pem"
		break
	fi
done
if [ ! -f "$IMAGE_DIR/isodir/etc/ssl/cert.pem" ]
then
	wget -O "$IMAGE_DIR/isodir/etc/ssl/cert.pem" https://curl.se/ca/cacert.pem
fi
cp "$IMAGE_DIR/isodir/etc/ssl/cert.pem" "$IMAGE_DIR/isodir/etc/ssl/certs/ca-certificates.crt"

find "$IMAGE_DIR/isodir/usr" -type f \( -perm -u+x -o -name '*.so' -o -name '*.so.*' \) | while read -r f
do
	if [ "$(head -c 4 "$f" | od -An -c | tr -d ' ')" = "177ELF" ]
	then
		"$SYSROOT/bin/x86_64-hermes-strip" --strip-unneeded "$f" || echo "not stripping $f"
	fi
done

echo "Image contents: $(du -sh "$IMAGE_DIR/isodir" | cut -f1)"

export QEMU_MEMORY=$((4096 + SMP * PROFILER_BUFFER_MIB))M

MONITOR_SOCKET=$SESSION_DIR/monitor.sock
QEMU_PIDFILE=$SESSION_DIR/qemu.pid
export QEMU_EXTRA_ARGS="-monitor unix:$MONITOR_SOCKET,server,nowait -pidfile $QEMU_PIDFILE ${QEMU_EXTRA_ARGS:-}"

KERNEL_COMMIT=$(git -C "$REPO_DIR" rev-parse HEAD 2>/dev/null || echo unknown)
if ! KERNEL_STATUS=$(git -C "$REPO_DIR" status --porcelain 2>/dev/null)
then
	KERNEL_DIRTY=unknown
elif [ -n "$KERNEL_STATUS" ]
then
	KERNEL_DIRTY=true
else
	KERNEL_DIRTY=false
fi

python3 "$PYTHON_PROFILER" _session-begin \
	--monitor-socket "$MONITOR_SOCKET" \
	--gdb-port 1234 \
	--elf "$KERNEL_BUILD/OS" \
	--output-root "$PROFILER_OUTPUT" \
	--qemu-pidfile "$QEMU_PIDFILE" \
	--kernel-commit "$KERNEL_COMMIT" \
	--kernel-dirty "$KERNEL_DIRTY" \
	"--build-flags=-DCMAKE_BUILD_TYPE=Release -DHERMES_PROFILER=ON -DHERMES_PROFILER_BUFFER_MIB=$PROFILER_BUFFER_MIB"

STATUS=0
VIRTIO=1 sh "$REPO_DIR/buildtools/run.sh" "$IMAGE_DIR" || STATUS=$?
if [ "$STATUS" -eq 0 ]
then
	python3 "$PYTHON_PROFILER" _session-end || :
fi
exit "$STATUS"
