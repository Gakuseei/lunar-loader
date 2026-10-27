#!/bin/sh
set -e
cd "$(dirname "$0")"

CC=x86_64-w64-mingw32-gcc
FLAGS="-O2 -s -static-libgcc -Iinclude"
WARNINGS="-Wall -Wextra"
MINHOOK=third_party/minhook/src

rm -rf dist build
mkdir -p dist/game/Mods build

for file in buffer hook trampoline hde/hde64; do
    $CC -O2 -c -I$MINHOOK -o "build/minhook_$(basename $file).o" "$MINHOOK/$file.c"
done
$CC $FLAGS $WARNINGS -shared -Ithird_party/minhook/include -o dist/game/version.dll \
    loader/*.c loader/version.def build/minhook_*.o -lbcrypt -lurlmon

for dir in mods/*/; do
    name=$(basename "$dir")
    mkdir -p "dist/game/Mods/$name"
    $CC $FLAGS $WARNINGS -shared -o "dist/game/Mods/$name/$name.dll" "$dir"*.c
done

cp LIESMICH.txt dist/game/
(cd dist/game && zip -qr ../MoonSweepersMods.zip .)
echo "dist/MoonSweepersMods.zip"
