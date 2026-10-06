#!/bin/sh
set -e

THREADS=${THREADS:-$(nproc 2>/dev/null || echo 4)}

GCC="gcc-16.2.0"
BINUTILS="binutils-2_43"
GDB="gdb-16.2"

CUR_DIR=$(cd "$(dirname "$0")" && pwd)
SYSROOT=$CUR_DIR/cross
WORKDIR=$(mktemp -d)

echo "Building cross-compiler with $THREADS thread(s)"

echo "Installing cross-compiler to $PREFIX"
echo "Building in directory $WORKDIR"

cd "$WORKDIR" || exit

# get and extract sources

if [ ! -d $BINUTILS ]
then
	wget https://github.com/memfault/binutils-gdb/archive/refs/tags/$BINUTILS.tar.gz
	tar -zxf $BINUTILS.tar.gz
fi

if [ ! -d $GCC ]
then
	wget https://github.com/gcc-mirror/gcc/archive/refs/tags/releases/$GCC.tar.gz
	tar -zxf $GCC.tar.gz
fi

# build and install libtools
cd $BINUTILS || exit
./configure --prefix=/usr --target=x86_64-elf --disable-nls --disable-werror --enable-default-execstack=no --with-sysroot=$SYSROOT
make -j "$THREADS" && DESTDIR="${SYSROOT}" make install
cd ..

# download gcc prerequisites
cd $GCC || exit
./contrib/download_prerequisites
cd ..

# build and install gcc
mkdir $GCC-elf-objs
cd $GCC-elf-objs || exit
CFLAGS_FOR_TARGET="-march=x86_64 -mabi-lp64d" \
CXXFLAGS_FOR_TARGET="-march=x86_64 -mabi-lp64d" \
../$GCC/configure --prefix="$PREFIX" \
  --target=x86_64-elf \
  --disable-nls \
  --enable-threads=posix \
  --disable-multilib \
  --enable-languages=c,c++ \
  --with-sysroot=$SYSROOT \
  --without-headers
make -j$THREADS all-gcc all-target-libgcc
DESTDIR="${SYSROOT}" make install-gcc install-target-libgcc
cd ..


cd "$CUR_DIR" || exit
rm -rf "$WORKDIR"
