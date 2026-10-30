#!/bin/sh

set -e

THREADS=${THREADS:-$(nproc 2>/dev/null || echo 4)}

DASH="dash-0.5.12"
DASH_SHA256="6a474ac46e8b0b32916c4c60df694c82058d3297d8b385b74508030ca4a8f28a"
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

if [ ! -f $DASH.tar.gz ]
then
	wget http://gondor.apana.org.au/~herbert/dash/files/$DASH.tar.gz
fi
echo "$DASH_SHA256  $DASH.tar.gz" | sha256sum -c -

rm -rf $DASH
tar -zxf $DASH.tar.gz

cd $DASH

sed -i 's/wait3(status, flags, NULL)/waitpid(-1, status, flags)/' src/jobs.c
./configure --host=$TARGET --prefix=/usr CFLAGS="-O2"
make -j "$THREADS"
make install-strip DESTDIR="$PROGRAMS"

echo "Installed dash to $PROGRAMS/usr/bin/dash"
