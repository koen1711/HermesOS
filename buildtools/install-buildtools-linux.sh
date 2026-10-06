#!/bin/sh
set -e

THREADS=${THREADS:-$(nproc 2>/dev/null || echo 4)}

GCC="gcc-16.2.0"
BINUTILS="binutils-2_43"
GDB="gdb-16.2"
TARGET="x86_64-hermes"

CUR_DIR=$(cd "$(dirname "$0")" && pwd)
PREFIX=$CUR_DIR/cross
SYSROOT=$CUR_DIR/cross
WORKDIR=$(mktemp -d)

echo "Building cross-compiler with $THREADS thread(s)"

echo "Installing cross-compiler to $PREFIX"
echo "Building in directory $WORKDIR"

# install mlibc headers into the sysroot so gcc can find them
bash "$CUR_DIR/mlibc-headers.sh"

cd "$WORKDIR" || exit

# get and extract sources

if [ ! -d binutils-gdb-$BINUTILS ]
then
	wget https://github.com/memfault/binutils-gdb/archive/refs/tags/$BINUTILS.tar.gz
	tar -zxf $BINUTILS.tar.gz
	patch -d binutils-gdb-$BINUTILS -p1 < "$CUR_DIR/patches/binutils-hermes.patch"
fi

if [ ! -d gcc-releases-$GCC ]
then
	wget https://github.com/gcc-mirror/gcc/archive/refs/tags/releases/$GCC.tar.gz
	tar -zxf $GCC.tar.gz
	patch -d gcc-releases-$GCC -p1 < "$CUR_DIR/patches/gcc-hermes.patch"
fi

# build and install libtools
cd binutils-gdb-$BINUTILS || exit
./configure --prefix="$PREFIX" --target=$TARGET --disable-nls --disable-werror --enable-default-execstack=no --with-sysroot="$SYSROOT"
make -j "$THREADS"
make install
cd ..

# gcc needs the freshly installed binutils on PATH
export PATH="$PREFIX/bin:$PATH"

# download gcc prerequisites
cd gcc-releases-$GCC || exit
./contrib/download_prerequisites
cd ..

# build and install gcc
mkdir $GCC-objs
cd $GCC-objs || exit
../gcc-releases-$GCC/configure --prefix="$PREFIX" \
  --target=$TARGET \
  --disable-nls \
  --enable-threads=posix \
  --disable-multilib \
  --enable-languages=c,c++ \
  --with-sysroot="$SYSROOT"
make -j "$THREADS" all-gcc all-target-libgcc
make install-gcc install-target-libgcc
cd ..


cd "$CUR_DIR" || exit
rm -rf "$WORKDIR"
