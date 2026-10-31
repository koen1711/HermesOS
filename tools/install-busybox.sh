#!/bin/sh

set -e

THREADS=${THREADS:-$(nproc 2>/dev/null || echo 4)}

BUSYBOX="busybox-1.38.0"
BUSYBOX_SHA256="34f9ea6ff8636f2c9241153b9114eefa9e65674a45318ae1ef95bb5f31c53bb2"
TARGET="x86_64-hermes"

CUR_DIR=$(cd "$(dirname "$0")" && pwd)
SYSROOT=$(cd "$CUR_DIR/../buildtools/cross" && pwd)
WORKDIR=$CUR_DIR/build
PROGRAMS=$(cd "$CUR_DIR/.." && pwd)/programs

export PATH="$SYSROOT/bin:$PATH"

if ! command -v $TARGET-gcc >/dev/null 2>&1
then
	echo "$TARGET-gcc not found, run buildtools/install-buildtools-linux.sh first"
	exit 1
fi

mkdir -p "$WORKDIR"
cd "$WORKDIR"

if [ ! -f $BUSYBOX.tar.bz2 ]
then
	wget https://busybox.net/downloads/$BUSYBOX.tar.bz2
fi
echo "$BUSYBOX_SHA256  $BUSYBOX.tar.bz2" | sha256sum -c -

rm -rf $BUSYBOX
tar -jxf $BUSYBOX.tar.bz2

cd $BUSYBOX

set_config() {
	sed -i -e "/^CONFIG_$1=/d" -e "/^# CONFIG_$1 is not set/d" .config
	echo "CONFIG_$1=$2" >> .config
}

unset_config() {
	sed -i -e "/^CONFIG_$1=/d" -e "/^# CONFIG_$1 is not set/d" .config
	echo "# CONFIG_$1 is not set" >> .config
}

make defconfig
set_config STATIC y
set_config CROSS_COMPILER_PREFIX "\"$TARGET-\""
unset_config TC
set_config FEATURE_SH_STANDALONE y
set_config FEATURE_PREFER_APPLETS y
set_config BUSYBOX_EXEC_PATH "\"/usr/bin/busybox\""

for applet in \
	BOOTCHARTD GETTY MICROCOM READAHEAD \
	MODPROBE_SMALL INSMOD RMMOD LSMOD MODPROBE DEPMOD MODINFO \
	ARP ETHER_WAKE NAMEIF NSLOOKUP ROUTE SLATTACH TELNET TELNETD \
	TRACEROUTE TRACEROUTE6 UDHCPC6 UDHCPD \
	FREE UPTIME RUNSV IONICE IPCS FSCK_MINIX MKFS_MINIX PIVOT_ROOT
do
	unset_config $applet
done

yes "" | make oldconfig >/dev/null

make -j "$THREADS"
install -D -m 755 busybox "$PROGRAMS/usr/bin/busybox"

echo "Installed busybox to $PROGRAMS/usr/bin/busybox"
