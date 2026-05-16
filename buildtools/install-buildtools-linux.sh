#!/bin/sh
set -e

THREADS=${THREADS:-$(nproc 2>/dev/null || echo 4)}

GCC="gcc-14.2.0"
BINUTILS="binutils-2.43"
GDB="gdb-16.2"

CUR_DIR=$(cd "$(dirname "$0")" && pwd)
PREFIX=$CUR_DIR/cross
WORKDIR=$(mktemp -d)

echo "Building cross-compiler with $THREADS thread(s)"

echo "Installing cross-compiler to $PREFIX"
echo "Building in directory $WORKDIR"

cd "$WORKDIR" || exit

# get and extract sources

if [ ! -d $BINUTILS ]
then
	curl --insecure -O https://ftp.gnu.org/gnu/binutils/$BINUTILS.tar.gz
	tar -zxf $BINUTILS.tar.gz
fi

if [ ! -d $GCC ]
then
	curl --insecure -O https://ftp.gnu.org/gnu/gcc/$GCC/$GCC.tar.gz
	tar -zxf $GCC.tar.gz
fi

# build and install libtools
cd $BINUTILS || exit
./configure --prefix="$PREFIX" --target=x86_64-elf --disable-nls --disable-werror --with-sysroot
make -j "$THREADS" && make install
cd ..

# download gcc prerequisites
cd $GCC || exit
./contrib/download_prerequisites
cd ..

# build and install gcc
mkdir $GCC-elf-objs
cd $GCC-elf-objs || exit
../$GCC/configure --prefix="$PREFIX" --target=x86_64-elf --disable-nls --enable-languages=c --without-headers
make all-gcc -j "$THREADS" && make all-target-libgcc -j "$THREADS" && make install-gcc && make install-target-libgcc
cd ..


cd "$CUR_DIR" || exit
rm -rf "$WORKDIR"
