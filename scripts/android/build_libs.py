#!/usr/bin/env python3
"""The native libraries of the Android app for one ABI, from a finished PC build (scripts/setup.py): the game library
libtomodachi_game.so (the lifted code the PC build made, compiled again for Android) and the runtime libmain.so, with the
Android NDK's clang. Into build-android/libs/<abi>/, beside libSDL3.so and libssbu_glslang.so; scripts/android/build_apk.py
packs them.

    python scripts/android/build_libs.py --abi arm64-v8a [--pc-build <build folder>] [--jobs N] [--host-only]

Work folder: build-android/work-<abi> (the PC build's sources and generated tables, its lifted C by a directory junction,
objects of its own). Needs: the NDK (ANDROID_NDK_ROOT, else the SDK's newest), deps/SDL3-<version> (headers),
build-android/libs/<abi>/libSDL3.so.
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC_EXT = ('.c', '.h', '.inc', '.cpp', '.py', '.def', '.txt', '.json', '.comp', '.frag', '.vert')
SRC_DIRS = ('plat', 'nocrt', 'fighter_sdk', 'ro_study', 'tools_audit')


def ndk_root():
    for d in [os.environ.get('ANDROID_NDK_ROOT')] + sorted(glob.glob(os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Android', 'Sdk', 'ndk', '*')), reverse=True):
        if d and os.path.isdir(os.path.join(d, 'toolchains', 'llvm')):
            return d
    sys.exit('the Android NDK was not found (ANDROID_NDK_ROOT)')


def stage(pc_work, work):
    """The PC build's work folder without its objects and binaries; gen_eh_o2 (the lifted C) by a junction."""
    os.makedirs(work, exist_ok=True)
    n = 0
    for name in os.listdir(pc_work):
        src, dst = os.path.join(pc_work, name), os.path.join(work, name)
        if os.path.isfile(src) and name.endswith(SRC_EXT):
            if not os.path.exists(dst) or os.path.getmtime(dst) < os.path.getmtime(src) or os.path.getsize(dst) != os.path.getsize(src):
                shutil.copy2(src, dst); n += 1
        elif os.path.isdir(src) and name in SRC_DIRS:
            shutil.copytree(src, dst, dirs_exist_ok=True)
    gen = os.path.join(work, 'gen_eh_o2')
    if not os.path.exists(gen):
        if os.name == 'nt':
            subprocess.run(['cmd', '/c', 'mklink', '/J', gen, os.path.join(pc_work, 'gen_eh_o2')], check=True, capture_output=True)
        else:
            os.symlink(os.path.join(pc_work, 'gen_eh_o2'), gen)
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--abi', choices=('arm64-v8a', 'x86_64'), default='arm64-v8a')
    ap.add_argument('--pc-build', default=os.path.join(REPO, 'build'))
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 8) - 2))
    ap.add_argument('--host-only', action='store_true', help='only libmain.so (the game library is not compiled again)')
    a = ap.parse_args()
    pc_work = os.path.join(os.path.abspath(a.pc_build), 'work')
    if not os.path.isdir(os.path.join(pc_work, 'gen_eh_o2')):
        sys.exit('no lifted code in %s: run scripts/setup.py first' % pc_work)
    work = os.path.join(REPO, 'build-android', 'work-' + a.abi)
    libs = os.path.join(REPO, 'build-android', 'libs', a.abi)
    print('staged %d changed files into %s' % (stage(pc_work, work), work), flush=True)
    ndk = ndk_root()
    exe = '.exe' if os.name == 'nt' else ''
    clang = os.path.join(ndk, 'toolchains', 'llvm', 'prebuilt', 'windows-x86_64' if os.name == 'nt' else 'linux-x86_64', 'bin', 'clang' + exe)
    sdl = sorted(glob.glob(os.path.join(REPO, 'deps', 'SDL3-*')))
    deps = os.environ.get('SSBU_DEPS') or os.path.join(REPO, 'deps')      # as scripts/setup.py
    env = {k: v for k, v in os.environ.items() if not k.startswith(('RECOMP_', 'SSBU_'))}
    env.update(RECOMP_TARGET='android', RECOMP_ANDROID_ABI=a.abi, RECOMP_CLANG=clang, RECOMP_GEN='gen_eh_o2', RECOMP_OBJ='obj_eh_o1',
               RECOMP_DLL='libtomodachi_game.so', RECOMP_INLINE='0', RECOMP_EXE='libmain.so', PYTHONUNBUFFERED='1',
               SDL3_ROOT=sdl[-1] if sdl else '', ANDROID_SDL3_LIB=os.path.join(libs, 'libSDL3.so'),
               VULKAN_HEADERS=os.environ.get('VULKAN_HEADERS') or os.path.join(deps, 'Vulkan-Headers'),
               IMGUI_ROOT=os.environ.get('IMGUI_ROOT') or os.path.join(deps, 'imgui'))
    state = os.path.join(os.path.abspath(a.pc_build), 'state.json')
    if os.path.isfile(state):
        import json
        env['RECOMP_BIN'] = os.path.join(os.path.abspath(a.pc_build), 'binaries', json.load(open(state)).get('game_version', '1.0.0'))
    py = sys.executable
    if not a.host_only:
        r = subprocess.run([py, 'build_opt.py', 'main', str(a.jobs)], cwd=work, env=env)
        if r.returncode:
            return r.returncode
    r = subprocess.run([py, 'build.py', 'host' if a.host_only else 'link'], cwd=work, env=env)
    if r.returncode:
        return r.returncode
    os.makedirs(libs, exist_ok=True)
    for n in ('libmain.so', 'libtomodachi_game.so'):
        if os.path.isfile(os.path.join(work, n)):
            shutil.copy2(os.path.join(work, n), os.path.join(libs, n))
    print('libraries in %s: %s' % (libs, ', '.join(sorted(os.listdir(libs)))))
    return 0


if __name__ == '__main__':
    sys.exit(main())
