#!/bin/sh
set -e
export PATH="/mingw64/bin:/usr/bin:$PATH"

GRUB_VERSION="2.12"
GRUB_URL="https://ftp.gnu.org/gnu/grub/grub-${GRUB_VERSION}.tar.xz"
INSTALL_DIR="/mingw64"
BUILD_DIR="/tmp/grub-build"

echo "Installing GRUB build dependencies..."
pacman -S --needed --noconfirm \
    mingw-w64-x86_64-gcc \
    mingw-w64-x86_64-python \
    mingw-w64-x86_64-freetype \
    mingw-w64-x86_64-pkg-config \
    mingw-w64-x86_64-gettext \
    mingw-w64-x86_64-gettext-tools \
    autoconf automake make bison

echo "Downloading GRUB ${GRUB_VERSION}..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
wget -q "$GRUB_URL" -O grub.tar.xz
tar xf grub.tar.xz
cd "grub-${GRUB_VERSION}"

echo "Configuring GRUB..."
./configure \
    --prefix="$INSTALL_DIR" \
    --target=x86_64 \
    --with-platform=efi \
    --disable-werror \
    --disable-nls \
    --disable-grub-mkfont

echo "Building GRUB (this will take 10-20 minutes)..."
make -j$(nproc)
make install

echo "GRUB installed successfully!"
grub-mkstandalone --version