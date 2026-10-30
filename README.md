# Tomodachi Life: Living the Dream, native PC port (beta)

A static recompilation of *Tomodachi Life: Living the Dream* (Nintendo Switch) into a native Windows program. The game's
own code is translated to C and compiled for your PC: no emulator runs underneath.

**This is a beta (0.1.0-beta).** You can play it from start to finish as far as we know, but expect rough edges: see
[Known issues](#known-issues).

**Nothing of the game is in this repository or in the release downloads.** You build it on your own PC from your own copy
of the game, dumped from your own Switch, with your own console keys. Not affiliated with or endorsed by Nintendo.

![The island](https://raw.githubusercontent.com/josiahg2502-glitch/TomodachiLife-recomp-files/main/screenshots/island.jpg)

| | |
|---|---|
| ![The apartments](https://raw.githubusercontent.com/josiahg2502-glitch/TomodachiLife-recomp-files/main/screenshots/apartments.jpg) | ![The food mart](https://raw.githubusercontent.com/josiahg2502-glitch/TomodachiLife-recomp-files/main/screenshots/food-mart.jpg) |

*Game 1.0.4, rendered at 4K in widescreen, shown at 1080p.*

## What you need

- Windows 10 or 11, 64-bit; about 15 GB free on the drive you install to; 8 GB of RAM or more (16 GB recommended).
- A GPU with OpenGL 4.5.
- Your game files, dumped from your Switch:
  - **the base game** (`.nsp`, `.nsz` or `.xci`; title id `010051F0207B2000`), and optionally
  - **the update 1.0.4** (`.nsp` or `.nsz`; `010051F0207B2800`, version `v262144`). Without it you get version 1.0.0.
- Your console's **`prod.keys`** (dumped with Lockpick_RCM). The installer finds it in `%USERPROFILE%\.switch` or in an
  emulator's key folder; key values are never copied or shown.

Both 1.0.0 and 1.0.4 run. Other updates are not supported yet.

## Install

1. Download **`Tomodachi-Life-PC.zip`** from the [newest release](https://github.com/josiahg2502-glitch/TomodachiLife-recomp/releases)
   and unpack it into a folder of its own, for example `C:\Games\TomodachiLife`. (GitHub's "Source code (zip)" holds the
   same files. A zip named `tomodachi-runtime-...` is not the game: it is a part the installer downloads by itself.)
2. Double-click **`Install Tomodachi Life.bat`**. It sets up a private copy of Python inside the folder (nothing is installed
   system-wide), then opens the installer window.
3. The window looks for your game files and keys (Downloads, Desktop, Documents and emulator folders). Check what it
   found, choose anything it missed, then press **Install**. A dump you already made (a folder with `exefs\` and
   `romfs\` inside) works too: the base game row's **Folder...** button; no keys are needed then.
4. Wait. The first install takes about 30 minutes to an hour: it unpacks your game, translates its code and compiles it.
   You can stop it and press **Resume** later; finished steps are not repeated.
5. Start the game with the **Tomodachi Life** shortcut on your desktop, or `Play Tomodachi Life.bat`.

The installer downloads only what it needs, each file checked against a fixed SHA-256: Python 3.13 (python.org), LLVM/clang
9 (the official build, repackaged as a zip), the prebuilt runtime of this release (our own code only), libopus and FFmpeg.

The **first start** prepares every shader of the game ("preparing shaders"): a few minutes, once. Later starts take seconds.

## The launcher

- **PLAY** starts the game.
- **GAME** shows the game version (1.0.0 or 1.0.4) and lets you choose another game folder.
- **SETTINGS**: window or full screen, resolution (up to 4K and above), widescreen (fits the monitor by default, ultrawide
  too), frame rate (60 fps by default), vsync, volume, language, Local Play online (saved in `build\config\graphics.ini`).
- **SAVE DATA**: back up and restore your save, or import a save dumped from your Switch (`Map.sav`, `Mii.sav`, `Player.sav`).
- **CONTROLS**: the controller it sees and the keyboard keys (`build\config\controllers.ini`).

**In the game**, **PC Settings** opens with **F1**, the controller's **HOME** / Guide button, or by holding **-** and **+**
together for a second (Steam Deck): screen, window size, resolution, widescreen, frame rate, vsync, volume, language and
Quit to desktop, on a page in the game's own style and with its own fonts. Played like the game's menus (d-pad or arrow
keys, A / Enter, B / Esc); the mouse works too. **Fast-forward:** hold **Tab**, or both stick buttons of a controller; the
speed (2x, 3x, 4x) is on that page.

Your save lives in `build\userdata` (or the install location you chose). **Playing 1.0.4 upgrades a 1.0.0 save; 1.0.0 cannot
read it afterwards.** Back it up first (SAVE DATA) if you may go back.

## Controls

The game is played as on a docked Switch.

| Switch | Controller | Keyboard |
|---|---|---|
| A / B / X / Y | the buttons with those letters; PlayStation: cross, circle, square, triangle (the game then shows those symbols) | Z / X / C / V |
| L / R, ZL / ZR | shoulders, triggers | Q / E, A / S |
| + / - | Start / Back | Enter / Backspace |
| Left stick | left stick | arrow keys |
| Right stick (camera) | right stick | F / T / H / G |
| D-pad | D-pad | J / I / L / K |

**Mouse** (off by default; PC Settings, **Mouse controls**): the hand follows the mouse. Left click = A, right click = B; the
wheel acts as ZL / ZR (in menus it moves the selection).
Keys and buttons can be changed in the game: PC Settings, **Controls** (pick the controller with left / right, choose a button,
press the new one; it holds for every controller of that kind), or in `build\config\controllers.ini`.

## Mods

Put each mod in its own folder in `build\mods` (made at the first start). Mods made for emulators or a modded Switch
(LayeredFS) work as they are: the game uses the first `romfs` folder inside each mod, for example
`build\mods\MyMod\romfs\...` or `build\mods\MyMod\atmosphere\contents\010051F0207B2000\romfs\...`. When two mods change the
same file, the one whose folder name comes first wins. To turn a mod off, move its folder out. `exefs` code patches cannot
work here: the game's code is translated ahead of time.

## Mii chatter (mod)

**Mii chatter** (off by default; PC Settings, **Mii chatter (mod)**) gives the Miis new things to say, in their own voice
and way of talking. Miis walking around now and then think a new thought, and when a Mii talks to you it may say a new
line instead of one of its usual ones, or add one after it. It learns your island as you play: who lives there, who moved
away, and the words you taught the Miis, and works them into what the Miis say. What it learned is kept in
`build\userdata\chatter_memory.txt`; delete that file to make it forget. English text and game version 1.0.4 only.

## Local Play (Mii exchange)

The game shares Miis through Local Play. Here Local Play runs over the network: in the launcher's SETTINGS, **LOCAL PLAY
ONLINE** = *Same network* for PCs at home, or *Host* (your game shows a room code in its title and copies it) and *Join*
(paste your friend's code) over the internet. It is off by default.

## Known issues

- Once, a freeze in the controller input was seen and not reproduced since. If the game stops responding, close it and
  send `build\work\play_*.log` of that session.
- Local Play over the internet worked on a simulated link (lag and lost packets) but not yet between two real home routers;
  some routers will need UPnP on. Sending a Mii works; exchanging creations is not tested yet.
- The launcher shows a plain background (the island picture of the dev builds is game art and is not shipped).
- Windows only for now.

When something goes wrong, the logs are in `build\logs` (install) and `build\work\play_*.log` (each game session).
**Flicker:** press **F12** while it happens. The game saves the next 8 pictures in `build\work\flicker` and describes the
frames after them in the session log (the game pauses a few seconds for that); send both.

## Building by hand

`python scripts/setup.py --dump <folder with exefs\ and romfs\>` builds everything from a dump folder, with LLVM/clang 9
and either the prebuilt runtime (`--runtime <zip>`) or the MSVC Build Tools; `python scripts/setup.py --help` lists the
steps and options. `tools/extract/dump_folder.py <base> [<update>] <folder>` makes that folder from your packages.
How the port works and what was done: [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## Credits and licence

Built on the runtime and tools of [ssbu-recomp](https://github.com/Denkishi/ssbu-recomp). GPL-3.0 (see `LICENSE`).
Third-party code: SDL 3 (zlib), Dear ImGui (MIT), stb (MIT / public domain), libopus (BSD), FFmpeg (LGPL, downloaded by the
installer), the open-source fonts in `runtime/fonts_open` (see the README there). The launcher's Mii import
(`launcher/mii_ltd.h`) is a port of the `.ltd` import of [ShareMii](https://github.com/Star-F0rce/ShareMii) by Star-F0rce
(GPL-3.0).
*Tomodachi Life* is a trademark of Nintendo. This project contains no Nintendo code or assets.
