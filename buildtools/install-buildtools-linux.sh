#!/bin/sh

echo -n "Please enter the amount of threads: "
read THREADS

GCC="gcc-8.2.0"
BINUTILS="binutils-2.31.1"
GDB="gdb-9.1"

CURRDIR=`pwd`
PREFIX=$CURRDIR/cross
WORKDIR=`mktemp -d`

echo "Installing cross-compiler to $PREFIX"
echo "Building in directory $WORKDIR"

cd "$WORKDIR"

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

if [ ! -d $GDB ]
then
	curl --insecure -O http://ftp.gnu.org/gnu/gdb/$GDB.tar.gz
	tar -zxf $GDB.tar.gz
fi

# build and install libtools
cd $BINUTILS
./configure --prefix="$PREFIX" --target=x86_64-elf --disable-nls --disable-werror --with-sysroot
make -j $THREADS && make install
cd ..

# download gcc prerequisites
cd $GCC
./contrib/download_prerequisites
cd ..

# build and install gcc
mkdir $GCC-elf-objs
cd $GCC-elf-objs
../$GCC/configure --prefix="$PREFIX" --target=x86_64-elf --disable-nls --enable-languages=c --without-headers
make all-gcc -j $THREADS && make all-target-libgcc -j $THREADS && make install-gcc && make install-target-libgcc
cd ..

# build and install GDB
mkdir ${GDB}-build
cd ${GDB}-build
../${GDB}/configure --prefix="$PREFIX" --target=x86_64-elf
make -j $THREADS && make install
cd ..

# build and install NASM
curl --insecure -O https://www.nasm.us/pub/nasm/releasebuilds/2.14.02/nasm-2.14.02.tar.gz
tar -zxf nasm-2.14.02.tar.gz
cd nasm-2.14.02
./configure --prefix="$PREFIX"
make -j $THREADS && make install
cd ..

cd "$CURRDIR"
rm -rf "$WORKDIR"

