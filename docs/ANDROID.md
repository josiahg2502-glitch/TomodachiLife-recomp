# Tomodachi Life on Android handhelds

A native Android app for Android gaming handhelds (Retroid, AYN, Anbernic and the like): the game's code is translated
to C and compiled from your own dump, as on Windows and Linux, and it draws with Vulkan. No emulator inside.

The app is built on a PC, from a finished Windows build. The game's files (about 7 GB) are not in the app: they are
copied to the device once.

## What you need

- **The device:** Android 10 or newer, a 64-bit ARM processor (arm64-v8a), a GPU with a Vulkan 1.1 driver, 8 GB of RAM
  (6 GB may be too little), about 8 GB of free storage (internal or SD card), and a controller (the built-in one of a
  handheld is fine). The game does not take touch input in this mode.
- **The PC:** a finished Windows build of this project (`scripts/setup.py`, or the folder the Windows installer made),
  Python 3 (the installer's is `deps\python\python.exe`), a Java JDK 17 or newer, and the Android SDK. Android Studio's SDK
  Manager installs what it needs: "NDK (Side by side)" 27.x, "CMake", "Android SDK Build-Tools", "Android SDK Platform 35".
  Keep the SDK in a folder without spaces in its path.

## Build the app (on the PC)

In the folder of your Windows build:

```bash
python scripts/android/build_libs.py --abi arm64-v8a
```

```bash
python scripts/android/build_apk.py --abi arm64-v8a --libs build-android/libs/arm64-v8a
```

The first command downloads SDL's source, builds it and the shader compiler for Android, and compiles the game (about 30
minutes); the second packs and signs `build-android/TomodachiLife-arm64-v8a.apk`. For the Android emulator or an x86
Chromebook, use `--abi x86_64` in both.

## Copy the game's files to the device

With the device connected by USB (USB debugging on), one command installs the app and copies the files:

```bash
python scripts/android/build_apk.py --abi arm64-v8a --libs build-android/libs/arm64-v8a --install --push build
```

Without USB debugging, copy by cable or SD card a folder named `TomodachiLife` to the top of the device's storage or SD
card, laid out like this:

```
TomodachiLife/
  game/     main.elf, sdk.elf, main.npdm   (from build/binaries/<version>/)
            romfs/                         (your dump's romfs folder)
  fonts/    the files of build/sysdata/fonts (or runtime/fonts_open)
  ui/       the files of build/work/ui     (the settings page's fonts)
```

Then install the APK (open it on the device, or `adb install`).

## Play

At the first start the app asks for "All files access" (Android 11 and newer; "Storage" on Android 10), so that it can
read the `TomodachiLife` folder: allow it, then start the app again. Saves are in `TomodachiLife/userdata` and stay when
the app is removed.

The first start prepares the game's shaders for your GPU and takes longer; later starts are quick.

File mods go in `TomodachiLife/mods/<mod name>/romfs/...` ([MODDING.md](MODDING.md)). **PC Settings** opens with the device's
**Back** button or by holding **-** and **+** together for a second; **Save now** and **Quit to desktop** are there too. When
you leave the app (Home, another app) the game waits; it goes on when you come back, unless Android has closed it to free
memory, so save first if you leave for long.

To update: build the app again from the new release on the PC (the two commands above) and install it over the old one.
Saves stay.

## Troubleshooting

- The log of the last start is `Android/data/org.tomodachirecomp.game/files/last_run.log` on the device's storage:
  attach it to a bug report.
- An `env.txt` file beside that log sets options for the next start, one `NAME=value` per line (the same variables as
  on Windows).
- "The game's files were not found": check that `TomodachiLife/game/main.elf` exists at the top of the storage or SD
  card, and that "All files access" is allowed for the app (Settings > Apps > Tomodachi Life > Permissions).

So far the app has been tested in the Android emulator on a PC (x86_64), not yet on a real handheld.
