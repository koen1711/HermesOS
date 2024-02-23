# ask how many threads to use
echo "How many threads to use?"
read THREADS

git clone https://git.savannah.gnu.org/git/grub.git
cd grub
./bootstrap

WORKING_DIR=$(pwd)

./configure --target=x86_64 --with-platform=efi --program-prefix="" --program-suffix="" --prefix=$WORKING_DIR/../grub-install
make -j$THREADS