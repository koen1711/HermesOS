#!/bin/sh

set -e

THREADS=${THREADS:-$(nproc 2>/dev/null || echo 4)}

LINUX="linux-6.12.114"
LINUX_SHA256="7a8589912bd7a8adb07fed074283652cf482a35facef8954554654f0154b4af3"

CUR_DIR=$(cd "$(dirname "$0")" && pwd)
SYSROOT=$(cd "$CUR_DIR/../buildtools/cross" && pwd)
WORKDIR=$CUR_DIR/build
HEADERS=$SYSROOT/linux-headers

mkdir -p "$WORKDIR"
cd "$WORKDIR"

# LINUX_SRC=/path/to/linux uses an existing kernel tree instead of downloading one
if [ -z "$LINUX_SRC" ]
then
	if [ ! -f $LINUX.tar.xz ]
	then
		wget https://cdn.kernel.org/pub/linux/kernel/v6.x/$LINUX.tar.xz
	fi
	echo "$LINUX_SHA256  $LINUX.tar.xz" | sha256sum -c -

	if [ ! -d $LINUX ]
	then
		tar -xf $LINUX.tar.xz
	fi
	LINUX_SRC=$WORKDIR/$LINUX
fi

rm -rf "$WORKDIR/linux-headers"
make -C "$LINUX_SRC" -j "$THREADS" ARCH=x86_64 INSTALL_HDR_PATH="$WORKDIR/linux-headers" headers_install

# mlibc wants the headers in their own directory, ports find them through the sysroot
rm -rf "$HEADERS"
mkdir -p "$HEADERS" "$SYSROOT/usr/include"
cp -r "$WORKDIR/linux-headers/include/." "$HEADERS/"
cp -r "$WORKDIR/linux-headers/include/." "$SYSROOT/usr/include/"

echo "Installed Linux headers to $HEADERS and $SYSROOT/usr/include"
