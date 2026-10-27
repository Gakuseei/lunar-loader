Lunar Loader for Moon Sweepers
https://github.com/Gakuseei/lunar-loader

Install
1. In Steam, right-click Moon Sweepers > Manage > Browse local files.
2. Extract the zip into that folder, so version.dll sits next to MoonSweepers.exe.
3. Start the game. SETTINGS now has a MODS tab.

Each mod is a folder inside Mods, for example Mods\AlwaysSun\AlwaysSun.dll.
To install a mod, put its folder into Mods. To remove it, delete the folder.

Linux and Steam Deck
In Steam, open Properties > General > Launch Options and enter:
WINEDLLOVERRIDES="version=n,b" %command%

In the game
SETTINGS > MODS lists every mod with an On/Off switch. Click a mod to see what it
does, change its settings or uninstall it.
Mods that sell upgrades put them into the upgrade matrix at your base, SERVICES tab.
The team wallet pays for them.

Online list
MODS > AVAILABLE has a switch for the online list. When you turn it on, the loader
downloads the list of mods from GitHub, and you can install, update and remove mods
from the menu. Every file is checked against the SHA-256 checksum in the list before
it is used. Changes apply after a restart. While the switch is off, the loader does
not go online.

Coop
The host runs the world: upgrades, machines, conveyors and solar power. Those mods
only work if the host has them. Light and camera run on each player's own game.

Problems
Logs: Mods\ModLoader.log and Mods\<mod>\<mod>.log. The previous session is in the
-prev.log files.
If the game does not start on Windows 11 after installing, Smart App Control may be
blocking version.dll, because it is not signed.
To remove everything, delete version.dll and the Mods folder.


Deutsch

Installieren
1. In Steam: Rechtsklick auf Moon Sweepers > Verwalten > Lokale Dateien durchsuchen.
2. Die Zip in diesen Ordner entpacken, sodass version.dll neben MoonSweepers.exe liegt.
3. Spiel starten. Unter SETTINGS gibt es jetzt den Reiter MODS.

Jede Mod ist ein Ordner in Mods, zum Beispiel Mods\AlwaysSun\AlwaysSun.dll.
Mod installieren: Ordner nach Mods legen. Mod entfernen: Ordner löschen.

Linux und Steam Deck
In Steam unter Eigenschaften > Allgemein > Startoptionen eintragen:
WINEDLLOVERRIDES="version=n,b" %command%

Im Spiel
SETTINGS > MODS zeigt jede Mod mit An/Aus. Ein Klick auf eine Mod zeigt, was sie
macht, ihre Einstellungen und den Knopf zum Deinstallieren.
Kaufbare Upgrades stehen in der Basis an der Upgrade-Matrix, Tab SERVICES. Bezahlt
wird aus der Teamkasse.

Online-Liste
Unter MODS > VERFÜGBAR lässt sich die Online-Liste einschalten. Dann lädt der Loader
die Liste der Mods von GitHub, und man kann Mods im Menü installieren, updaten und
entfernen. Jede Datei wird vor dem Einsatz mit der SHA-256-Prüfsumme aus der Liste
verglichen. Änderungen wirken nach einem Neustart. Solange der Schalter aus ist, geht
der Loader nicht ins Internet.

Koop
Der Host rechnet die Welt: Upgrades, Maschinen, Förderbänder und Solarstrom. Diese
Mods wirken nur, wenn der Host sie hat. Licht und Kamera rechnet jeder Spieler selbst.

Probleme
Logs: Mods\ModLoader.log und Mods\<Mod>\<Mod>.log, die vorherige Runde in den
-prev.log Dateien.
Startet das Spiel unter Windows 11 nach der Installation nicht, blockiert eventuell
Smart App Control die version.dll, weil sie nicht signiert ist.
Alles entfernen: version.dll und den Ordner Mods löschen.
