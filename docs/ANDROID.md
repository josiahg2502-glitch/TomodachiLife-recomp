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
  Python 3, a Java JDK 17 or newer, and the Android SDK with the NDK r27 (Android Studio's SDK Manager installs both:
  "NDK (Side by side)" 27.x and "Android SDK Build-Tools").

## Build the app (on the PC)

```bash
python scripts/android/build_libs.py --abi arm64-v8a
```

```bash
python scripts/android/build_apk.py --abi arm64-v8a --libs build-android/libs/arm64-v8a
```

The first command compiles the game for Android (about 30 minutes); the second packs and signs
`build-android/TomodachiLife-arm64-v8a.apk`. For the Android emulator or an x86 Chromebook, use `--abi x86_64` in both.

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
```

Then install the APK (open it on the device, or `adb install`).

## Play

At the first start the app asks for "All files access" (Android 11 and newer; "Storage" on Android 10), so that it can
read the `TomodachiLife` folder: allow it, then start the app again. Saves are in `TomodachiLife/userdata` and stay when
the app is removed.

The first start prepares the game's shaders for your GPU and takes longer; later starts are quick.

## Troubleshooting

- The log of the last start is `Android/data/org.tomodachirecomp.game/files/last_run.log` on the device's storage:
  attach it to a bug report.
- An `env.txt` file beside that log sets options for the next start, one `NAME=value` per line (the same variables as
  on Windows).
- "The game's files were not found": check that `TomodachiLife/game/main.elf` exists at the top of the storage or SD
  card, and that "All files access" is allowed for the app (Settings > Apps > Tomodachi Life > Permissions).

So far the app has been tested in the Android emulator on a PC (x86_64), not yet on a real handheld.
