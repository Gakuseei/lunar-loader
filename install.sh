#!/bin/sh
set -e
cd "$(dirname "$0")"
GAME="${1:-$HOME/.local/share/Steam/steamapps/common/Moon Sweepers}"

place() {
    mkdir -p "$(dirname "$2")"
    cp "$1" "$2.new"
    mv -f "$2.new" "$2"
}

place dist/game/version.dll "$GAME/version.dll"
for dll in dist/game/Mods/*/*.dll; do
    place "$dll" "$GAME/${dll#dist/game/}"
done
echo "installed into $GAME"
