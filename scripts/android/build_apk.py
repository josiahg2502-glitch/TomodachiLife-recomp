#!/usr/bin/env python3
"""The Android app, made on the PC: SDL's Java glue (deps/SDL3-<version>/android-project) and the native libraries of one ABI
(tools/lifter/build.py with RECOMP_TARGET=android: libmain.so, libtomodachi_game.so; SDL3's libSDL3.so; the glslang shim
libssbu_glslang.so), packed with the Android SDK's build tools and signed with a key made on this PC (build-android/).
Optionally installed on the device or emulator that adb sees, and the game's own files copied to it: they are not in the
app (6 GB), but in a TomodachiLife folder at the top of the device's storage (runtime/android_main.c).

    python scripts/android/build_apk.py --abi arm64-v8a --libs <folder of the .so files> [--out <apk>] [--install]
                                        [--push <build folder of a finished PC build>]
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PACKAGE = 'org.tomodachirecomp.game'
LIBS = ('libmain.so', 'libtomodachi_game.so', 'libSDL3.so', 'libssbu_glslang.so')
MIN_SDK, TARGET_SDK = 29, 35


def sdk_root():
    for d in (os.environ.get('ANDROID_SDK_ROOT'), os.environ.get('ANDROID_HOME'), os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Android', 'Sdk')):
        if d and os.path.isdir(os.path.join(d, 'build-tools')):
            return d
    sys.exit('the Android SDK was not found (ANDROID_SDK_ROOT)')


def jdk_bin():
    cands = [os.path.join(os.environ['JAVA_HOME'], 'bin')] if os.environ.get('JAVA_HOME') else []
    cands += sorted(glob.glob(r'C:\Program Files\Microsoft\jdk-*\bin') + glob.glob(r'C:\Program Files\Eclipse Adoptium\jdk-*\bin'), reverse=True)
    for d in cands:
        if os.path.isfile(os.path.join(d, 'javac.exe' if os.name == 'nt' else 'javac')):
            return d
    sys.exit('a Java JDK (17 or newer) was not found (JAVA_HOME)')


def run(argv, **kw):
    r = subprocess.run(argv, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit('%s failed:\n%s' % (os.path.basename(argv[0]), (r.stdout + r.stderr)[-3000:]))
    return r.stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--abi', choices=('arm64-v8a', 'x86_64'), default='arm64-v8a')
    ap.add_argument('--libs', required=True, help='folder holding ' + ', '.join(LIBS))
    ap.add_argument('--out', help='the APK (default build-android/TomodachiLife-<abi>.apk)')
    ap.add_argument('--version', default=open(os.path.join(REPO, 'version.txt')).read().strip())
    ap.add_argument('--install', action='store_true', help='adb install -r the APK')
    ap.add_argument('--push', help='a finished build folder: its converted dump, RomFS and fonts are copied to the device')
    a = ap.parse_args()
    sdk = sdk_root()
    bt = sorted(glob.glob(os.path.join(sdk, 'build-tools', '*')))[-1]
    jar = sorted(glob.glob(os.path.join(sdk, 'platforms', 'android-*', 'android.jar')))[-1]
    jdk = jdk_bin()
    ext = '.exe' if os.name == 'nt' else ''
    bat = '.bat' if os.name == 'nt' else ''
    sdl = sorted(glob.glob(os.path.join(REPO, 'deps', 'SDL3-*', 'android-project', 'app', 'src', 'main', 'java')))
    if not sdl:
        sys.exit('SDL3\'s source (deps/SDL3-<version>) was not found: its Java glue is part of the app')
    missing = [n for n in LIBS if not os.path.isfile(os.path.join(a.libs, n))]
    if missing:
        sys.exit('missing in %s: %s' % (a.libs, ', '.join(missing)))
    out = os.path.abspath(a.out or os.path.join(REPO, 'build-android', 'TomodachiLife-%s.apk' % a.abi))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    tmp = tempfile.mkdtemp(prefix='apk_', dir=os.path.dirname(out))
    try:
        # SDL's Java classes -> classes.dex
        java = glob.glob(os.path.join(sdl[-1], 'org', 'libsdl', 'app', '*.java'))
        os.makedirs(os.path.join(tmp, 'classes'))
        run([os.path.join(jdk, 'javac' + ext), '-source', '8', '-target', '8', '-nowarn', '-encoding', 'UTF-8', '-Xlint:-options',
             '-classpath', jar, '-d', os.path.join(tmp, 'classes')] + java)
        classes = glob.glob(os.path.join(tmp, 'classes', '**', '*.class'), recursive=True)
        os.makedirs(os.path.join(tmp, 'dex'))
        run([os.path.join(bt, 'd8' + bat), '--release', '--min-api', str(MIN_SDK), '--lib', jar, '--output', os.path.join(tmp, 'dex')] + classes)
        # the manifest -> the base of the APK
        code = sum(int(x) * k for x, k in zip((a.version.split('.') + ['0', '0', '0'])[:3], (10000, 100, 1)))
        run([os.path.join(bt, 'aapt2' + ext), 'link', '-o', os.path.join(tmp, 'base.apk'), '--manifest', os.path.join(REPO, 'scripts', 'android', 'AndroidManifest.xml'),
             '-I', jar, '--min-sdk-version', str(MIN_SDK), '--target-sdk-version', str(TARGET_SDK), '--version-code', str(max(code, 1)), '--version-name', a.version])
        # + classes.dex + the native libraries (stored: zipalign puts them on page boundaries)
        unaligned = os.path.join(tmp, 'unaligned.apk')
        with zipfile.ZipFile(os.path.join(tmp, 'base.apk')) as zin, zipfile.ZipFile(unaligned, 'w', zipfile.ZIP_DEFLATED) as zout:
            for i in zin.infolist():
                zout.writestr(i, zin.read(i.filename))
            zout.write(os.path.join(tmp, 'dex', 'classes.dex'), 'classes.dex')
            for n in LIBS:
                zout.write(os.path.join(a.libs, n), 'lib/%s/%s' % (a.abi, n), compress_type=zipfile.ZIP_STORED)
        aligned = os.path.join(tmp, 'aligned.apk')
        run([os.path.join(bt, 'zipalign' + ext), '-P', '16', '-f', '4', unaligned, aligned])
        # signed with this PC's key (made once and kept: an update must carry the same certificate), in Python (apksign.py)
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import apksign
        apksign.sign(aligned, out, os.path.join(REPO, 'build-android', 'signing'))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print('%s: %.1f MB' % (out, os.path.getsize(out) / 1048576.0))
    adb = os.path.join(sdk, 'platform-tools', 'adb' + ext)
    if a.install:
        print(run([adb, 'install', '-r', out]).strip())
    if a.push:
        import json
        build = os.path.abspath(a.push)
        state = json.load(open(os.path.join(build, 'state.json')))
        # the TomodachiLife folder at the top of the device's storage, where runtime/android_main.c looks (with the "all files
        # access" it asks for at the first start); adb push cannot make folders under Android/data on Android 11 and newer
        dst = '/sdcard/TomodachiLife'
        run([adb, 'shell', 'mkdir', '-p', dst + '/game', dst + '/fonts'])
        binaries = os.path.join(build, 'binaries', state.get('game_version', '1.0.0'))
        for n in os.listdir(binaries):
            run([adb, 'push', os.path.join(binaries, n), dst + '/game/'])
        fonts = os.path.join(build, 'sysdata', 'fonts')
        fonts = fonts if os.path.isdir(fonts) else os.path.join(REPO, 'runtime', 'fonts_open')
        for n in os.listdir(fonts):
            if n.lower().endswith(('.ttf', '.bfttf', '.otf')):
                run([adb, 'push', os.path.join(fonts, n), dst + '/fonts/'])
        ui = os.path.join(build, 'work', 'ui')         # the settings page's fonts, taken from the game's files (scripts/launcher_art.py)
        if os.path.isdir(ui):
            run([adb, 'push', ui, dst + '/'])
        print('copying the RomFS (6 GB, 66,000 files: about 10 minutes over USB) to the device ...', flush=True)
        run([adb, 'shell', 'rm', '-rf', dst + '/game/romfs'])
        run([adb, 'push', state['dump']['romfs'], dst + '/game/'])
        print('the game\'s files are on the device: %s (the app asks for access to your files at its first start)' % dst)
    return 0


if __name__ == '__main__':
    sys.exit(main())
