# Lunar Loader

A mod loader and a set of mods for [Moon Sweepers](https://store.steampowered.com/app/5335510/).

The loader is a `version.dll` that sits next to `MoonSweepers.exe`. It loads every mod in the `Mods` folder when the game starts and adds a MODS tab to the settings screen. It does not need BepInEx or MelonLoader, which do not load this Unity 6 IL2CPP build.

## Install

1. In Steam, right-click Moon Sweepers > Manage > Browse local files.
2. Extract the loader zip into that folder, so `version.dll` sits next to `MoonSweepers.exe`.
3. Put mod folders into `Mods`, for example `Mods/AlwaysSun/AlwaysSun.dll`.

On Linux and Steam Deck, set the launch options to `WINEDLLOVERRIDES="version=n,b" %command%`.

To remove everything, delete `version.dll` and the `Mods` folder.

## In the game

SETTINGS > MODS lists every installed mod with an On/Off switch. Clicking a mod opens its page with a description, its settings and an uninstall button.

MODS > AVAILABLE has an online list. It is off until you switch it on. Then the loader downloads [`mods.ini`](mods.ini) from this repository and you can install, update and remove mods from the menu. Each download is checked against the SHA-256 checksum in that file before it is used, and changes apply after a restart. Mods by other modders appear under Community.

Mods can sell upgrades through the loader. They show up in the upgrade matrix at your base, in a SERVICES tab, and the team wallet pays for them. Bought levels are also written to `Mods/upgrades.ini`, so a world saved once without the mods gets them back.

In coop the host runs the world. Upgrades, machines, conveyors and solar power only change if the host has the mod. Light and camera run on each player's game.

## Mods

| Mod | What it does |
| --- | --- |
| Always Sun | Daylight everywhere on the moon and full sun for solar panels, day and night. Power and light can be switched separately. |
| Conveyor Upgrades | Belts, splitters and mergers move 1.5x, 2x or 3x as fast. Costs 150, 450 and 1,200. |
| Faster Machines | Pressure chambers and stone presses work 1.5x, 2x or 3x as fast at the same power use. Costs 200, 600 and 1,500. |
| J.O.O.N.A.S. Upgrades | Drive (2x, 3x, 5x speed), size (1.5x, 2x, 2.5x, cleans a wider path) and tank (150, 300, 600 kg, bigger capsules, catchers take one every 5, 3 or 1.5 s instead of 9 s). |
| Steady Vehicle Camera | While driving, the camera stays where you put it. |

With Conveyor Upgrades, the belt surface animation and belt sound keep their normal speed.

## Build

You need `x86_64-w64-mingw32-gcc` and `zip`.

```sh
./build.sh
```

This writes the loader, every mod and the release zips to `dist/`. `./install.sh [game folder]` copies the build into a Steam install.

## Writing mods

See [MODDING.md](MODDING.md). The mod interface is one header, [`include/modapi.h`](include/modapi.h).

## License

MIT, see [LICENSE](LICENSE). The loader includes [MinHook](https://github.com/TsudaKageyu/minhook) under its BSD 2-Clause license, see `third_party/minhook/LICENSE.txt`.
