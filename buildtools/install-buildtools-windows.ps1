# Requires: MSYS2 (gcc, make, curl, tar in PATH) or WSL

$THREADS = Read-Host "Please enter the amount of threads"

$GCC      = "gcc-14.2.0"
$BINUTILS = "binutils-2.43"
$GDB      = "gdb-16.2"

$CUR_DIR  = Get-Location
$PREFIX   = "$CUR_DIR\cross"
$WORKDIR  = Join-Path $env:TEMP ("build_" + [System.IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Path $WORKDIR | Out-Null

Write-Host "Installing cross-compiler to $PREFIX"
Write-Host "Building in directory $WORKDIR"

Set-Location $WORKDIR

# Download and extract sources
if (-not (Test-Path $BINUTILS)) {
    curl.exe --insecure -O "https://ftp.gnu.org/gnu/binutils/$BINUTILS.tar.gz"
    tar -zxf "$BINUTILS.tar.gz"
}

if (-not (Test-Path $GCC)) {
    curl.exe --insecure -O "https://ftp.gnu.org/gnu/gcc/$GCC/$GCC.tar.gz"
    tar -zxf "$GCC.tar.gz"
}

# Build and install binutils
Set-Location $BINUTILS
bash -c "./configure --prefix='$($PREFIX -replace '\\','/') --target=x86_64-elf --disable-nls --disable-werror --with-sysroot"
bash -c "make -j $THREADS && make install"
Set-Location ..

# Download GCC prerequisites
Set-Location $GCC
bash -c "./contrib/download_prerequisites"
Set-Location ..

# Build and install GCC
New-Item -ItemType Directory -Path "$GCC-elf-objs" -Force | Out-Null
Set-Location "$GCC-elf-objs"

$prefixUnix = $PREFIX -replace '\\', '/'
bash -c "../$GCC/configure --prefix='$prefixUnix' --target=x86_64-elf --disable-nls --enable-languages=c --without-headers"
bash -c "make all-gcc -j $THREADS && make all-target-libgcc -j $THREADS && make install-gcc && make install-target-libgcc"
Set-Location ..

# Cleanup
Set-Location $CUR_DIR
Remove-Item -Recurse -Force $WORKDIR