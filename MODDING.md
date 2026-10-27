# Writing a mod for Lunar Loader

A mod is a Windows DLL that exports `ModInit`. The loader looks for `Mods/<Name>/<Name>.dll`, loads it when the game's scripting runtime has started and calls `ModInit` on the main thread. Everything a mod can use from the loader is in [`include/modapi.h`](include/modapi.h).

## A minimal mod

```c
#include "modapi.h"

static const ModApi *api;
static const int *loud;

MOD_EXPORT int ModInit(const ModApi *modApi, Mod *mod)
{
    api = modApi;
    api->describe(mod, "Beispiel", "Example", "Schreibt eine Zeile ins Log.", "Writes a line to the log.");
    loud = api->toggle(mod, "Loud", "Laut", "Loud", 0, 0, NULL);
    api->log(mod, *loud ? "HELLO" : "hello");
    return 1;
}
```

Build it with MinGW:

```sh
x86_64-w64-mingw32-gcc -O2 -s -shared -Iinclude -o Example.dll example.c
```

Put `Example.dll` into `Mods/Example/` and start the game. The mod shows up in SETTINGS > MODS, and its log is `Mods/Example/Example.log`.

Return 0 from `ModInit`, or call `api->incompatible(mod, reason)`, when something the mod needs is missing. The menu then greys the mod out and shows the reason.

## What the API gives you

`describe`, `toggle`, `choice` and `note` fill the mod's page in the MODS tab. Settings are saved in `Mods/<Name>/settings.ini`. A setting with the `MOD_WORLD` flag can only be changed by the host. `enabled` and `onEnabled` tell you whether the player switched the mod off.

`klass`, `method` and `code` look up game classes and methods by assembly, namespace and name. `code` returns the native address of a method, which you can call directly or pass to `hook`. `export` returns any `il2cpp_*` function of `GameAssembly.dll`.

`hook` replaces a game function with your own and gives you a pointer to the original. Only one mod can hook a given function. If a second mod tries, `hook` returns 0.

`find` searches the game code for a byte pattern such as `"F3 0F 58 15 ?? ?? ?? ??"` and returns the address only if the pattern occurs exactly once. `write` patches bytes at such an address.

`upgrade` adds a buyable node to the SERVICES tab of the upgrade matrix. Call it from `ModInit` with a unique id, titles and summaries in German and English, an icon number from the game's `MoonUpgradeTree.Glyph` list, the number of levels and one price per level. `upgradeLevel(world, id)` returns the level bought in that world. The game handles paying from the team wallet, saving and syncing purchases in coop.

`isHost` tells you whether this player runs the world. In coop, only the host's game simulates machines, robots and power.

Names of game classes and methods can be read from a dump of `GameAssembly.dll` and `global-metadata.dat`, for example with [Cpp2IL](https://github.com/SamboyCoding/Cpp2IL). The game uses IL2CPP metadata version 108, which needs a current development build of Cpp2IL.

## Getting your mod into the online list

The online list is [`mods.ini`](mods.ini) in this repository. To add your mod, open a pull request with a new section:

```ini
[Example]
name=Example
version=1.0.0
author=Your name
community=1
en=Writes a line to the log.
de=Schreibt eine Zeile ins Log.
url=https://github.com/you/example/releases/download/v1.0.0/Example.dll
sha256=<sha256 of Example.dll>
```

The section name is the folder name. `url` must be an `https` link to the DLL itself, and `sha256` must match that file, otherwise the loader refuses to install it. Every mod is checked before it is listed, so link your source code in the pull request if it is public.
