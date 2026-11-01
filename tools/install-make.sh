#!/bin/sh

set -e

THREADS=${THREADS:-$(nproc 2>/dev/null || echo 4)}

MAKE_PKG="make-4.4.1"
MAKE_SHA256="dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3"
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

if [ ! -f $MAKE_PKG.tar.gz ]
then
	wget https://ftp.gnu.org/gnu/make/$MAKE_PKG.tar.gz
fi
echo "$MAKE_SHA256  $MAKE_PKG.tar.gz" | sha256sum -c -

rm -rf $MAKE_PKG
tar -zxf $MAKE_PKG.tar.gz

cd $MAKE_PKG

./configure --build="$(sh build-aux/config.guess)" --host=x86_64-hermes-linux-gnu --prefix=/usr \
	--without-guile --disable-nls --disable-load \
	CC=$TARGET-gcc AR=$TARGET-ar RANLIB=$TARGET-ranlib STRIP=$TARGET-strip CFLAGS="-O2 -std=gnu17"
make -j "$THREADS"
make install-strip DESTDIR="$PROGRAMS"

echo "Installed make to $PROGRAMS/usr/bin/make"
