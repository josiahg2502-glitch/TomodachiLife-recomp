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
| A (confirm) | A; PlayStation: cross (the game then shows the PlayStation symbols) | Enter or Space |
| B (back) | B; PlayStation: circle | Esc, Backspace or B |
| X (menu) / Y (residents) | X / Y; PlayStation: square / triangle | X or M / Y or R |
| L / R, ZL / ZR | shoulders, triggers | Q / E, Z / C |
| + / - | Start / Back | = / - (or the number pad's + / -) |
| Left stick (the hand) | left stick | W A S D or the arrow keys |
| Right stick (camera) | right stick | J / I / L / K |
| D-pad | D-pad | number pad 4 / 8 / 6 / 2 |

Keys move the hand like a stick: a tap nudges it (half speed for the first 0.3 s, for small targets), a held key crosses
the screen, and diagonals are no faster than straight moves.

**Mouse** (off by default; PC Settings, **Mouse controls**): the hand follows the mouse. Left click = A, right click = B; the
wheel zooms the camera on the island. In menus and dialogs (the X menu, Settings, questions) pointing at a button selects it,
a click presses it (a click beside the buttons does nothing) and the wheel moves the selection; on the keyboard for names
and words, pointing selects a key, a left click types it and a right click deletes.
Keys and buttons can be changed in the game: PC Settings, **Controls** (pick the controller with left / right, choose a button,
press the new one; it holds for every controller of that kind), or in `build\config\controllers.ini`, where a button can have
up to three keys (`A = ENTER, SPACE`). A keyboard section still holding the old default keys (Z / X / C / V) is moved to the
keys above by itself; one you changed is kept.

## Mods

Put each mod in its own folder in `build\mods` (made at the first start). Mods made for emulators or a modded Switch
(LayeredFS) work as they are: the game uses the first `romfs` folder inside each mod, for example
`build\mods\MyMod\romfs\...` or `build\mods\MyMod\atmosphere\contents\010051F0207B2000\romfs\...`. When two mods change the
same file, the one whose folder name comes first wins. To turn a mod off, move its folder out. `exefs` code patches cannot
work here: the game's code is translated ahead of time.

## Mii chatter (mod)

**Mii chatter** (PC Settings, **Mii chatter (mod)**: Off, Some or Lots; off by default) gives the Miis more to say, in
their own voice and way of talking, while the game's own lines stay most of what they say:

- When a Mii talks to you, it says its usual line and may add a new one in a second balloon.
- Miis walking around sometimes think a new thought, and what they say while busy (stretching, aerobics, painting...)
  sometimes comes in a new version.
- When two Miis chat (**English Mii chats (mod)**, on by default), some conversations are in English instead of Mii-speak.

It learns your island as you play: who lives there, who just moved in (a new Mii is learned once the game saves), who
moved away, and the words you taught the Miis. It knows which Mii is speaking, so a thought, or what a Mii tells you when you check on
them, can be about another resident by name, never about the one speaking. It also knows each Mii as the game does: their personality (worked out from
their personality sliders the way the game does, with the game's own names and descriptions), their pronouns, their
birthday and who they are to each other (partners, family, friends), so thoughts and chats fit who is talking, birthdays
coming up get mentioned and Miis gossip about the island's couples and families. Everything it learns and writes is kept in `build\userdata`
(`chatter_memory.txt`, `chatter_lines.txt`, `chatter_knowledge.txt`); delete those files to make it forget. The library of
new lines keeps growing as you play, and lines that were said often make way for new ones. English text and game
version 1.0.4 only.

**Free AI for the Miis (optional):** put a language model file in `build\ai` and the new lines are written by it, on your
PC and offline. It was made for Qwen2.5 1.5B Instruct (`qwen2.5-1.5b-instruct-q4_k_m.gguf`, 1.12 GB, Apache 2.0, from
huggingface.co/Qwen/Qwen2.5-1.5B-Instruct-GGUF). It knows Tomodachi Life from the game's own catalogues (its foods,
facilities, goods, quirks and trips, read from your game files), the real calendar (the weekday, holidays coming up) and
the real world, and it writes from what the mod learned about your island. It runs on a few low-priority CPU threads,
only while the CPU has room to spare (it waits while the game loads), and keeps lines ready, so the game never waits for
it. Every line is checked before a Mii may say it. Without a model file the mod writes from its own templates, and Miis
chat in Mii-speak as usual.

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
