#!/bin/sh
set -e
cd "$(dirname "$0")"

VERSION=1.0.0
CC=x86_64-w64-mingw32-gcc
FLAGS="-O2 -s -static-libgcc -Iinclude -Wl,--no-insert-timestamp"
WARNINGS="-Wall -Wextra"
MINHOOK=third_party/minhook/src

rm -rf dist build
mkdir -p dist/game/Mods dist/release build

for file in buffer hook trampoline hde/hde64; do
    $CC -O2 -c -I$MINHOOK -o "build/minhook_$(basename $file).o" "$MINHOOK/$file.c"
done
$CC $FLAGS $WARNINGS -shared -Ithird_party/minhook/include -o dist/game/version.dll \
    loader/*.c loader/version.def build/minhook_*.o -lbcrypt -lurlmon

cp README.txt dist/game/
cp LICENSE dist/game/LICENSE.txt
cp third_party/minhook/LICENSE.txt dist/game/MinHook-LICENSE.txt
(cd dist/game && zip -qr "../release/LunarLoader-$VERSION.zip" version.dll Mods README.txt LICENSE.txt MinHook-LICENSE.txt)

for dir in mods/*/; do
    name=$(basename "$dir")
    mkdir -p "dist/game/Mods/$name"
    $CC $FLAGS $WARNINGS -shared -o "dist/game/Mods/$name/$name.dll" "$dir"*.c
    cp "dist/game/Mods/$name/$name.dll" dist/release/
    (cd dist/game/Mods && zip -qr "../../release/$name-$VERSION.zip" "$name")
    sum=$(sha256sum "dist/game/Mods/$name/$name.dll" | cut -d' ' -f1)
    sed -i "/^\[$name\]/,/^\[/ s/^sha256=.*/sha256=$sum/" mods.ini
done

(cd dist/game && zip -qr "../release/LunarLoader-$VERSION-all-mods.zip" .)
cp mods.ini dist/release/
ls dist/release
