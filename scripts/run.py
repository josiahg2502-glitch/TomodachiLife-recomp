#!/usr/bin/env python3
"""Start the built game: the launcher (TomodachiLauncher.exe) of the install location, or the game itself.

    python scripts/run.py                     the launcher ("Tomodachi Life Launcher.exe" does this)
    python scripts/run.py --direct            the game without the launcher, until you quit (the log in <build>\\logs)
    python scripts/run.py --direct --userdata <folder> --seconds 60     a test run with a save folder you can lose

The install location is <repo>\\build or the folder chosen in the installer window (build_location.txt). The saves are in
<build>\\userdata (the launcher's SAVE DATA backs them up into <build>\\saves).

Linux (./play.sh runs this): the game of <repo>/build-linux (setup.py --target linux) on Vulkan, its output in the terminal,
the saves in ~/.local/share/tomodachi-recomp/userdata.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_location  # noqa: E402

EXE, GAME_DLL, LAUNCHER = 'tomodachi_native.exe', 'tomodachi_game.dll', 'TomodachiLauncher.exe'
LINUX = sys.platform.startswith('linux')
DATA = os.path.join(os.environ.get('XDG_DATA_HOME') or os.path.expanduser('~/.local/share'), 'tomodachi-recomp')      # Linux saves


def main_linux(a):
    build = os.path.abspath(a.build or os.path.join(REPO, 'build-linux'))
    work = os.path.join(build, 'work')
    if not (os.path.isfile(os.path.join(work, 'tomodachi_native')) and os.path.isfile(os.path.join(build, 'state.json'))):
        print('The game is not built yet (looked in %s): run scripts/linux/install.sh <your dump folder> first.' % build)
        return 1
    gone = [f for f in ('tomodachi_game.so', 'ssbu_glslang.so') if not os.path.isfile(os.path.join(work, f))]
    if gone:
        print('The game files are incomplete: %s missing in %s. Run scripts/linux/install.sh again: it makes them again.' % (', '.join(gone), work))
        return 1
    with open(os.path.join(build, 'state.json')) as f:
        state = json.load(f)
    env = {k: v for k, v in os.environ.items() if not k.startswith(('RECOMP_', 'SSBU_'))}
    env['SSBU_ROMFS'] = state['dump']['romfs']
    env['SSBU_USERDATA'] = os.path.abspath(a.userdata or os.path.join(DATA, 'userdata'))
    env['SSBU_RENDERER'] = 'vk'                    # (the OpenGL renderer is Windows only)
    env['SSBU_OS_CONSOLE_CORES'] = '1'
    env['SSBU_KEYBOARD_PLAYER'] = '1'
    if os.path.isdir(os.path.join(build, 'sysdata', 'fonts')):
        env['SSBU_FONTS'] = os.path.join(build, 'sysdata', 'fonts')
    for kv in a.env:
        k, _, v = kv.partition('=')
        env[k] = v
    os.makedirs(env['SSBU_USERDATA'], exist_ok=True)
    cmd = [os.path.join(work, 'tomodachi_native'), os.path.join(build, 'binaries', state.get('game_version', '1.0.0'), 'main.elf'), str(a.seconds)]
    return subprocess.run(cmd, cwd=work, env=env).returncode


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--direct', action='store_true', help='start the game itself, not the launcher')
    ap.add_argument('--build', help='build folder (default: the install location of build_location.txt, else <repo>/build)')
    ap.add_argument('--userdata', help='the save folder (default <build>\\userdata, the launcher\'s)')
    ap.add_argument('--seconds', type=int, default=0, help='stop after this many seconds (0 = play until you quit)')
    ap.add_argument('--env', action='append', default=[], help='NAME=VALUE for the game, may be repeated')
    a = ap.parse_args()
    if LINUX:
        return main_linux(a)
    build = os.path.abspath(a.build or build_location.get())
    work = os.path.join(build, 'work')
    if not (os.path.isfile(os.path.join(work, EXE)) and os.path.isfile(os.path.join(build, 'state.json'))):
        print('The game is not built yet (looked in %s): start "Tomodachi Life Launcher.exe" and press INSTALL first.' % build)
        return 1
    gone = [f for f in (GAME_DLL, 'SDL3.dll') if not os.path.isfile(os.path.join(work, f))]
    if gone:
        print('The game files are incomplete: %s missing in %s.\n'
              'Usually an antivirus removed it (Windows Security > Virus & threat protection > Protection history: restore it\n'
              'and allow it), or the installation stopped before the end. Start "Tomodachi Life Launcher.exe" and press\n'
              'INSTALL: it makes the missing files again in a few minutes.' % (', '.join(gone), work))
        return 1
    if not a.direct and os.path.isfile(os.path.join(work, LAUNCHER)):
        subprocess.Popen([os.path.join(work, LAUNCHER)], cwd=work, close_fds=True)
        return 0
    with open(os.path.join(build, 'state.json')) as f:
        state = json.load(f)
    env = {k: v for k, v in os.environ.items() if not k.startswith(('RECOMP_', 'SSBU_'))}
    romfs = state['dump']['romfs']
    if not os.path.isdir(romfs) and os.path.isdir(os.path.join(build, 'dump', 'romfs')):
        romfs = os.path.join(build, 'dump', 'romfs')       # the folder was moved: state.json still names the old place
    env['SSBU_ROMFS'] = romfs
    env['SSBU_USERDATA'] = os.path.abspath(a.userdata or os.path.join(build, 'userdata'))
    env['SSBU_OS_CONSOLE_CORES'] = '1'
    env['SSBU_KEYBOARD_PLAYER'] = '1'
    if os.path.isdir(os.path.join(build, 'sysdata', 'fonts')):
        env['SSBU_FONTS'] = os.path.join(build, 'sysdata', 'fonts')
    for kv in a.env:
        k, _, v = kv.partition('=')
        env[k] = v
    os.makedirs(env['SSBU_USERDATA'], exist_ok=True)
    logs = os.path.join(build, 'logs')
    os.makedirs(logs, exist_ok=True)
    log = os.path.join(logs, time.strftime('game_%Y%m%d_%H%M%S.log'))
    print('Playing. The game log goes to %s. Close the game window to quit.' % log)
    exe = os.path.join(work, EXE)
    try:                                       # as the launcher: its copy, for which the install prepared the shaders (kept per exe name)
        shutil.copy2(exe, os.path.join(work, 'tomodachi_play.exe'))
        exe = os.path.join(work, 'tomodachi_play.exe')
    except OSError:                            # (the copy is running: the exe itself)
        pass
    cmd = [exe, os.path.join(build, 'binaries', state.get('game_version', '1.0.0'), 'main.elf'), str(a.seconds)]
    t0 = time.time()
    with open(log, 'wb') as out:
        p = subprocess.run(cmd, cwd=work, env=env, stdout=out, stderr=subprocess.STDOUT)
    print('game exited with code %d after %.0f s' % (p.returncode, time.time() - t0))
    return p.returncode


if __name__ == '__main__':
    sys.exit(main())
