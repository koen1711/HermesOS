# How to install buildtools?

## Linux only

### Gentoo


### Debian
    
```bash
    sudo apt-get install build-essential
    sudo apt-get install cmake
    sudo apt-get install libgmp3-dev
    sudo apt-get install libmpfr-dev
    sudo apt-get install libmpc-dev
    export PREFIX="$HOME/opt/cross"
    export TARGET=i686-elf
    export PATH="$PREFIX/bin:$PATH"
    cd binutils/ && mkdir build && cd build
    ../configure --target=$TARGET --prefix="$PREFIX" --with-sysroot --disable-nls --disable-werror
    make
    make install
    cd ../../gcc/ && mkdir build && cd build
    ../configure --target=$TARGET --prefix="$PREFIX" --disable-nls --enable-languages=c,c++ --without-headers
    make all-gcc && make all-target-libgcc
    make install-gcc && make install-target-libgcc
```