#!/usr/bin/env python3
"""The native libraries of the Android app for one ABI, from a finished PC build (scripts/setup.py): the game library
libtomodachi_game.so (the lifted code the PC build made, compiled again for Android) and the runtime libmain.so, with the
Android NDK's clang. Into build-android/libs/<abi>/, beside libSDL3.so and libssbu_glslang.so; scripts/android/build_apk.py
packs them.

    python scripts/android/build_libs.py --abi arm64-v8a [--pc-build <build folder>] [--jobs N] [--host-only]

Work folder: build-android/work-<abi> (the PC build's sources and generated tables, its lifted C by a directory junction,
objects of its own). Needs: the NDK (ANDROID_NDK_ROOT, else the SDK's newest) and the SDK's CMake. What is missing of the
rest is made once: SDL's source in deps/SDL3-<version> (downloaded), Vulkan-Headers and Dear ImGui (as the installer gets
them), libSDL3.so (with the SDK's CMake) and libssbu_glslang.so (runtime/vk_glslang_shim.c + the NDK's glslang source).
"""
import argparse
import glob
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'scripts'))      # install.py: its downloads (TOOLS, fetch, download)
SDL3_SOURCE = ('https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz',
               '9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3')      # GitHub's digest of the release asset
SRC_EXT = ('.c', '.h', '.inc', '.cpp', '.py', '.def', '.txt', '.json', '.comp', '.frag', '.vert')
SRC_DIRS = ('plat', 'nocrt', 'fighter_sdk', 'ro_study', 'tools_audit')


def ndk_root():
    for d in [os.environ.get('ANDROID_NDK_ROOT')] + sorted(glob.glob(os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Android', 'Sdk', 'ndk', '*')), reverse=True):
        if d and os.path.isdir(os.path.join(d, 'toolchains', 'llvm')):
            return d
    sys.exit('the Android NDK was not found (ANDROID_NDK_ROOT)')


def sdl3_source():
    """deps/SDL3-<version>: SDL's source, for its headers and the Java glue of the app (scripts/android/build_apk.py)."""
    have = glob.glob(os.path.join(REPO, 'deps', 'SDL3-*', 'android-project'))
    if have:
        return os.path.dirname(sorted(have)[-1])
    url, sha = SDL3_SOURCE
    name = os.path.basename(url)
    deps = os.path.join(REPO, 'deps')
    os.makedirs(deps, exist_ok=True)
    part = os.path.join(deps, name + '.part')
    print('downloading %s' % url, flush=True)
    import install
    install.download(url, part)
    if hashlib.sha256(open(part, 'rb').read()).hexdigest() != sha:
        os.remove(part)
        sys.exit('%s does not match its SHA-256: refused' % url)
    with tarfile.open(part) as t:
        t.extractall(deps)
    os.remove(part)
    return os.path.join(deps, name[:-len('.tar.gz')])


def sdl3_lib(ndk, abi, src, out):
    """libSDL3.so for the ABI, with the SDK's CMake."""
    if os.path.isfile(out):
        return
    bins = sorted(glob.glob(os.path.join(os.path.dirname(os.path.dirname(ndk)), 'cmake', '*', 'bin')))
    if not bins:
        sys.exit('the Android SDK has no CMake: add "CMake" in its SDK Manager')
    exe = '.exe' if os.name == 'nt' else ''
    cmake, b = os.path.join(bins[-1], 'cmake' + exe), os.path.join(REPO, 'build-android', 'sdl3-' + abi)
    print('building libSDL3.so for %s ...' % abi, flush=True)
    subprocess.run([cmake, '-S', src, '-B', b, '-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + os.path.join(bins[-1], 'ninja' + exe),
                    '-DCMAKE_TOOLCHAIN_FILE=' + os.path.join(ndk, 'build', 'cmake', 'android.toolchain.cmake'), '-DANDROID_ABI=' + abi,
                    '-DANDROID_PLATFORM=android-29', '-DCMAKE_BUILD_TYPE=Release', '-DSDL_SHARED=ON', '-DSDL_STATIC=OFF', '-DSDL_TESTS=OFF',
                    '-DSDL_EXAMPLES=OFF'], check=True, stdout=subprocess.DEVNULL)
    subprocess.run([cmake, '--build', b], check=True, stdout=subprocess.DEVNULL)
    shutil.copy2(os.path.join(b, 'libSDL3.so'), out)
    shutil.rmtree(b, ignore_errors=True)


def glslang_lib(ndk, abi, clang, out):
    """libssbu_glslang.so for the ABI: the Vulkan renderer's GLSL -> SPIR-V, from the glslang source in the NDK."""
    if os.path.isfile(out):
        return
    g = os.path.join(ndk, 'sources', 'third_party', 'shaderc', 'third_party', 'glslang')
    if not os.path.isfile(os.path.join(g, 'Android.mk')):
        sys.exit('this NDK has no glslang source (%s): use NDK r27' % g)
    o = os.path.join(REPO, 'build-android', 'glslang-' + abi)
    os.makedirs(o, exist_ok=True)
    print('building libssbu_glslang.so for %s ...' % abi, flush=True)
    subprocess.run([os.path.join(ndk, 'ndk-build.cmd' if os.name == 'nt' else 'ndk-build'), 'NDK_PROJECT_PATH=null', 'APP_BUILD_SCRIPT=' + os.path.join(g, 'Android.mk'),
                    'APP_ABI=' + abi, 'APP_PLATFORM=android-29', 'APP_STL=c++_static', 'NDK_OUT=obj', 'NDK_LIBS_OUT=libs',      # (relative: make cannot take a path with spaces)
                    'APP_MODULES=glslang SPIRV OSDependent OGLCompiler HLSL', '-j%d' % (os.cpu_count() or 8)], cwd=o, check=True, stdout=subprocess.DEVNULL)
    target = {'x86_64': 'x86_64', 'arm64-v8a': 'aarch64'}[abi]
    subprocess.run(['++'.join(os.path.splitext(clang)), '--target=%s-linux-android29' % target, '-O2', '-shared', '-fPIC',
                    '-x', 'c', os.path.join(REPO, 'runtime', 'vk_glslang_shim.c'), '-x', 'none', '-I', g, '-o', out] +
                   [os.path.join(o, 'obj', 'local', abi, 'lib%s.a' % n) for n in ('glslang', 'SPIRV', 'HLSL', 'OGLCompiler', 'OSDependent')] +
                   ['-static-libstdc++', '-llog', '-lm', '-Wl,--no-undefined', '-Wl,-soname,libssbu_glslang.so', '-s'], check=True)
    shutil.rmtree(o, ignore_errors=True)


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
    sdl = sdl3_source()
    os.makedirs(libs, exist_ok=True)
    sdl3_lib(ndk, a.abi, sdl, os.path.join(libs, 'libSDL3.so'))
    glslang_lib(ndk, a.abi, clang, os.path.join(libs, 'libssbu_glslang.so'))
    deps = os.environ.get('SSBU_DEPS') or os.path.join(REPO, 'deps')      # as scripts/setup.py
    vulkan = os.environ.get('VULKAN_HEADERS') or os.path.join(deps, 'Vulkan-Headers')
    imgui = os.environ.get('IMGUI_ROOT') or os.path.join(deps, 'imgui')
    if not (os.path.isdir(vulkan) and os.path.isdir(imgui)):
        import install
        vulkan = vulkan if os.path.isdir(vulkan) else install.fetch('vulkan-headers')
        imgui = imgui if os.path.isdir(imgui) else install.fetch('imgui')
    env = {k: v for k, v in os.environ.items() if not k.startswith(('RECOMP_', 'SSBU_'))}
    env.update(RECOMP_TARGET='android', RECOMP_ANDROID_ABI=a.abi, RECOMP_CLANG=clang, RECOMP_GEN='gen_eh_o2', RECOMP_OBJ='obj_eh_o1',
               RECOMP_DLL='libtomodachi_game.so', RECOMP_INLINE='0', RECOMP_EXE='libmain.so', PYTHONUNBUFFERED='1',
               SDL3_ROOT=sdl, ANDROID_SDL3_LIB=os.path.join(libs, 'libSDL3.so'), VULKAN_HEADERS=vulkan, IMGUI_ROOT=imgui)
    state = os.path.join(os.path.abspath(a.pc_build), 'state.json')
    if os.path.isfile(state):
        import json
        env['RECOMP_BIN'] = os.path.join(os.path.abspath(a.pc_build), 'binaries', json.load(open(state)).get('game_version', '1.0.0'))
    py = sys.executable
    # the self-tests' reference data: an install with the prebuilt Windows exe did not make it (scripts/setup.py step_tables)
    if not os.path.isfile(os.path.join(work, 'sdktest_data.h')):
        import install
        subprocess.run([py, 'gen_sdktest.py'], cwd=work, env=dict(env, SUYU_ROOT=install.fetch_suyu()), check=True)
    if not os.path.isfile(os.path.join(work, 'quad_tests.h')):
        subprocess.run([py, 'gen_quad_tests.py'], cwd=work, env=env, check=True)
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
