# Tomodachi Life on Linux and Steam Deck

A native Linux build: the game's code is translated to C and compiled on your PC from your own dump, as on Windows, and
it draws with Vulkan. No Wine, no emulator.

## What you need

- A 64-bit x86 PC or a Steam Deck, 8 GB of RAM or more (16 GB recommended), about 15 GB free.
- A GPU with a Vulkan 1.3 driver (Mesa RADV / ANV, NVIDIA's driver).
- Your game, dumped from your own Switch, as a folder with `exefs/` and `romfs/` inside (the base game, or the base game
  with the 1.0.4 update applied). The Windows installer makes such a folder from `.nsp` / `.xci` files and your keys;
  on Linux, `tools/extract/` does the same (`romfs_extract.py`, `nca_exefs.py`, with your keys in `~/.switch/prod.keys`).

## Install

```bash
git clone https://github.com/josiahg2502-glitch/TomodachiLife-recomp.git
cd TomodachiLife-recomp
scripts/linux/install.sh ~/path/to/your/dump
```

`install.sh` does everything, and can be run again after an interruption (finished steps are skipped):

1. **System packages** (asks for your password once): clang, SDL3, FFmpeg, glslang, the Vulkan loader, Python
   (pacman, apt or dnf). Where the distribution has no SDL3 yet (Ubuntu before 25.04, Debian 12), SDL3 3.4.18 is built
   from source into `deps/sdl3-linux`.
2. **The build** (40 minutes to about 2 hours, depending on the CPU and the disk): the game's code is translated and
   compiled, and every shader of the game is prepared for Vulkan, so the first start has much less left to compile.
3. **"Tomodachi Life" in your application menu.**

## Play

From the application menu, or `./play.sh`. Saves are in `~/.local/share/tomodachi-recomp/userdata`; the output of the
last start is in `~/.local/share/tomodachi-recomp/last_run.log` (attach it to a bug report).

The controls and settings are the same as on Windows (README.md, "Controls"): any controller SDL knows, keyboard and
mouse; PC Settings in the game's own menu.

## Updates

`play.sh` looks for a new release at most once a day and shows a notice when there is one. To install it, choose "Update to
the newest release" on the game's application menu entry (right click), or run `scripts/linux/update.sh`: it moves this
folder to the new release and builds what changed. Your dump, saves and settings stay.

## Steam Deck

SteamOS is read-only, and its updates remove what is installed into it, so the build runs in a distrobox container
(SteamOS 3.5 and newer include distrobox). In Desktop Mode, open Konsole in this folder (in your home folder) and run:

```bash
distrobox create --name tomodachi --image docker.io/library/archlinux:latest
distrobox enter tomodachi
scripts/linux/install.sh ~/path/to/your/dump
```

The game starts at 720p on a Steam Deck (PC Settings, **Resolution**, changes it). The application menu entry starts the game
inside the container. To play in Game Mode, add it to Steam as a
non-Steam game (Steam > Add a game > Add a non-Steam game > Tomodachi Life).

## Troubleshooting

- `scripts/setup.py --target linux --status` shows which steps are done; `--redo <step>` repeats one.
- The build's logs are in `build-linux/logs/`.
- `install_deps.sh --no-system` skips the package manager if you install the packages yourself.
- The game says the processor is drawing it, or it runs at a few frames per second and seems to freeze at new scenes: your
  graphics card has no Vulkan 1.3 driver, so Mesa's software one (llvmpipe) took over. `using device` in `last_run.log` names the
  device. Install the card's driver: the package `mesa-vulkan-drivers` or `vulkan-radeon` / `vulkan-intel`, or NVIDIA's own driver.

So far this build has been tested under WSL2 on a PC, without a real GPU, not yet on a real Linux graphics driver or a Steam Deck.
Reports are very welcome: attach `last_run.log`, and `tomodachi_native_crash.txt` from `build-linux/work` if the game closed by itself.
