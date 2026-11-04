#!/usr/bin/env python3
"""One-click install: from your own game files to a playable build. Started by the launcher's INSTALL screen (or the older installer window).

    python scripts/install.py [--no-gui] [--progress-json] [--dry-run] [--base F] [--update F] [--keys F]
                              [--dump DIR] [--firmware DIR] [--runtime ZIP | --no-runtime] [--jobs N]
                              [--no-shortcuts] [--no-self-update] [--prepare-only] [--build-dir DIR]

build\\ below is the install location: <repo>\\build, or the folder chosen with --build-dir (the installer window's "Install
location"), remembered in build_location.txt at the repository root (scripts/build_location.py) and used by every later run.

Every step is skipped when its result is already there, so the installer can be started again after a stop (what is done is
also written to build\\install_state.json; scripts/setup.py keeps its own build\\state.json). Steps, in order:
 1. update    scripts/update.py brings this folder to the newest version of the repository (--no-self-update skips it). If
              files changed, install.py starts itself again once so the new code runs. setup.py then rebuilds only what
              changed: its `stage` step copies only changed files (copy_if_changed) and the compilers redo only what is older
              than its sources; update.py names the earliest setup step to repeat and it is passed as `setup.py --from`.
 2. precheck  scripts/precheck.py: Windows, CPU, RAM, disk, Vulkan, codecs, build tools.
 3. packages  Python packages (requirements.txt) through pip (no --user inside the private deps\\python).
 4. inputs    scripts/find_inputs.py looks for what you did not give: prod.keys (emulator folders), firmware, the base game,
              its update 1.0.4 (optional: the base game alone is 1.0.0). A prod.keys outside ~/.switch is not copied: the
              tools get its path in SSBU_PROD_KEYS. Key values are never printed.
 5. dump      build\\dump from the base game (+ update) (tools/extract/dump_folder.py; .nsz and .nca are read by
              romfs_extract.py), or the dump folder you have.
              The firmware is optional: it gives your console's own system fonts; without it the open-source fonts of
              runtime\\fonts_open are used.
 6. tools     LLVM/clang 9 and the prebuilt runtime (or SDL3, Vulkan-Headers, suyu, MSVC to build everything).
 7. media     libopus (the game's Opus sounds) into deps\\opus; FFmpeg (movies where Windows cannot play them) into
              deps\\ffmpeg, both SHA-256 checked.
 8. build     scripts/setup.py: deps stage convert tables lift sdk compile link prepare.
 9. shortcuts "Tomodachi Life" on the desktop and in the Start menu, "Tomodachi Life - Installer" in the Start menu (scripts/shortcuts.py).
(A 10th step cleared the downloaded-file mark over the scripts and exes, through PowerShell with its execution policy
bypassed; with the launcher's hidden PowerShell it made antivirus programs warn about the release, 2026-11-02. Nothing needs
it: the mark does not stop Python or the programs the installer starts.)

--dry-run says what each step would do ("would ...") and changes nothing: no downloads, no writes outside build\\logs.

--progress-json adds, besides the plain lines, lines that start with "##ssbu " followed by one JSON object (for the window):
  {"ev":"plan","steps":[{"id","title","est_s"}]}                           first
  {"ev":"step","id","state":"start|done|skip|fail|dry","secs","msg"}       per step (setup.py's steps have their own ids)
  {"ev":"progress","id","done","total"[,"est":true]}                        optional; est:true = elapsed / expected seconds
  {"ev":"done","ok":true|false,"msg"} or {"ev":"error","msg","hint"}        last
After a self-update the second run does not repeat the plan and the update step.

Ideas (a one-click installer that updates itself, finds the user's keys / dumps in emulator folders, makes shortcuts and unblocks
files) after WindowSmash's tools/install_windowsmash.ps1 by SNO2207 (no licence file; nothing copied); this is a separate
implementation.
"""
import argparse
import collections
import hashlib
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import ssl
import time
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DEPS = os.path.join(REPO, 'deps')
sys.path.insert(0, HERE)        # the python.org embeddable package does not put the script's folder on sys.path
import build_location  # noqa: E402
# The install location: the build folder (dump, work, logs, saves, the .nsz decompression cache). --build-dir, else the one
# remembered in build_location.txt (the installer window's "Install location"), else <repo>\build. set_build() changes it.
BUILD = build_location.get()
STATE_FILE = os.path.join(BUILD, 'install_state.json')


def set_build(path):
    global BUILD, STATE_FILE
    BUILD = os.path.abspath(path)
    STATE_FILE = os.path.join(BUILD, 'install_state.json')


# the MSVC Build Tools through winget: --passive shows Microsoft's own progress window (--quiet showed nothing at all and
# could wait on a prompt nobody saw); 5-15 minutes, about 3 GB on C:
MSVC_WINGET = ('winget install --id Microsoft.VisualStudio.2022.BuildTools -e --accept-package-agreements --accept-source-agreements '
               '--override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"')
PKGS = ['zstandard', 'Pillow', 'pycryptodome', 'lz4']
# name -> (url, sha256 or None, folder in deps, file that proves the folder is complete). A None hash refuses the download
# until the maintainer has checked the archive and written its hash here.
# Third-party files this project hosts itself, in the release installer-tools of its files repository (keep it): the same bytes as
# their first source, checked by the same SHA-256 (LLVM, FFmpeg, libopus, two suyu source files)
MIRROR = 'https://github.com/josiahg2502-glitch/TomodachiLife-recomp-files/releases/download/installer-tools/'
TOOLS = {   # checked 2026-10-19: SDL3 equals GitHub's own asset digest; LLVM's installer has a good signature of the
            # LLVM release key B6C8 F982 82B9 44E3 B0D5 C253 0FC3 042E 345A D05D (LLVM-9.0.0-win64.exe.sig)
    'sdl3': ('https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-devel-3.4.18-VC.zip',
             '78d84602ae616cfe26b33a73b7d3b9b6b56e8a6650e53d92c9c6ca938f747acd', 'SDL3', os.path.join('include', 'SDL3', 'SDL.h')),
    'vulkan-headers': ('https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.329.zip',
                       '0f55079543cfbf023bda65f81bd2d5a1bab581f31e5c33cf9fd781bd754ebaba', 'Vulkan-Headers', os.path.join('include', 'vulkan', 'vulkan.h')),
    # LLVM 9 as a plain zip (no installer: no administrator prompt, and nothing to uninstall when another LLVM is on the
    # PC, which made the official installer fail with "uninstall failed"): the files of the official LLVM-9.0.0-win64.exe
    # (SHA-256 71ed99a6...8863, signed) extracted with 7-Zip, unchanged, only what the build uses (README.txt inside); the
    # zip of ssbu-recomp's release, kept on this project's files repository (MIRROR) so the install never depends on another's
    'llvm': (MIRROR + 'llvm-9.0.0-win64-portable.zip',
             '0e0c34046589a73efef3e2a4c06335815681afbc7c883b665cdbed98669a22e3', 'llvm', os.path.join('bin', 'clang.exe')),
    # Dear ImGui (MIT), optional: GitHub publishes no digest for tag archives, so this hash is the archive checked on
    # 2026-10-19 (2,021,831 bytes)
    'imgui': ('https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b.zip',
              'fd37507c8476a6d14cc7c4b352401f31bcbd0f0d995d35390811e968c466f46e', 'imgui', 'imgui.h'),
}

# The prebuilt runtime package of this version (scripts/make_release.py): (URL, SHA-256 of the zip). It sits beside the
# other installer downloads (MIRROR), not on the game's release page: unpacked there by hand it passed for the game.
# (None, None) until the maintainer has uploaded the package made from this commit and written its hash here; the
# download is refused when the hash does not match, and setup.py uses the package only with the tree it was built from.
RUNTIME_RELEASE = (MIRROR + 'tomodachi-runtime-252c1f293f.zip', '4a562d88cb0bba8f019c881c1303d0739896c9244efc9a4d52cf3ec71c2f1bec')

SETUP_STEPS = ['deps', 'stage', 'convert', 'tables', 'lift', 'sdk', 'compile', 'link', 'prepare']
# expected seconds, measured 2026-10-28 on a PC with 9 jobs, 16 GB (compile scales with 12 / jobs)
EST = {'update': 10, 'precheck': 10, 'packages': 30, 'inputs': 10, 'dump': 180, 'tools': 30, 'media': 20,
       'deps': 5, 'stage': 3, 'convert': 3, 'tables': 5, 'lift': 30, 'sdk': 15, 'compile': 1260,
       'link': 30, 'prepare': 900, 'shortcuts': 3}      # prepare: every shader compiled (829 s on an RTX 3060)
TITLES = {'update': 'Update this program', 'precheck': 'Check this PC', 'packages': 'Python packages',
          'inputs': 'Find your game files', 'dump': 'Prepare the game files', 'tools': 'Build tools', 'media': 'Sound and movies',
          'deps': 'Check tools and game files', 'stage': 'Lay out the sources', 'convert': 'Convert the game code',
          'tables': 'Tables', 'lift': 'Translate the game code', 'sdk': 'System library',
          'compile': 'Compile the game', 'link': 'Link the game', 'prepare': 'Shaders and launcher',
          'shortcuts': 'Shortcuts'}


class Stop(Exception):
    """A step cannot go on: msg says why, hint says what to do; action names a fix the window offers as a button
    ('msvc': install the MSVC Build Tools in a visible console)."""
    def __init__(self, msg, hint='', action=''):
        Exception.__init__(self, msg)
        self.msg, self.hint, self.action = msg, hint, action


def say(s=''):
    print(str(s).encode('ascii', 'replace').decode('ascii'), flush=True)


class Prog:
    """The `##ssbu {json}` lines of --progress-json."""
    def __init__(self, on):
        self.on, self.t0 = on, {}

    def emit(self, **kw):
        if self.on:
            print('##ssbu ' + json.dumps(kw), flush=True)

    def start(self, sid):
        self.t0[sid] = time.time()
        self.emit(ev='step', id=sid, state='start', secs=0, msg='')

    def end(self, sid, state, msg='', secs=None):
        if secs is None:
            secs = time.time() - self.t0.get(sid, time.time())
        self.emit(ev='step', id=sid, state=state, secs=round(secs, 1), msg=msg)


class State:
    """build\\install_state.json: what finished (so a second start skips it), the inputs used. Not written in a dry run."""
    def __init__(self, dry):
        self.dry, self.d = dry, {'done': {}, 'inputs': {}}
        try:
            with open(STATE_FILE) as f:
                self.d.update(json.load(f))
        except (OSError, ValueError):
            pass

    def save(self):
        if self.dry:
            return
        os.makedirs(BUILD, exist_ok=True)
        tmp = STATE_FILE + '.tmp'
        with open(tmp, 'w') as f:
            json.dump(self.d, f, indent=1)
        os.replace(tmp, STATE_FILE)

    def done(self, sid, secs, msg):
        self.d['done'][sid] = {'secs': round(secs, 1), 'at': time.strftime('%Y-%m-%d %H:%M'), 'msg': msg[:200]}
        self.save()


def secs_of(text):
    """'19.3s' or '3m05s' (setup.py's fmt) -> seconds."""
    m = re.fullmatch(r'(?:(\d+)m)?(\d+(?:\.\d+)?)s', text.strip())
    return (int(m.group(1) or 0) * 60 + float(m.group(2))) if m else 0.0


def stream(cmd, env=None, cwd=None, on_line=None, hb=None, prefix=''):
    """Run a child, print its output line by line as it comes. hb() -> (id, done, total) | None is called every ~15 s with no
    output, for the estimated-progress events. Returns (exit code, last lines)."""
    e = dict(os.environ if env is None else env)
    e.setdefault('PYTHONUNBUFFERED', '1')
    e.setdefault('PYTHONIOENCODING', 'utf-8')
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=e, cwd=cwd, text=True, encoding='utf-8',
                         errors='replace', bufsize=1)
    q = queue.Queue()

    def reader():
        for line in p.stdout:
            q.put(line)
        q.put(None)
    threading.Thread(target=reader, daemon=True).start()
    tail, last = collections.deque(maxlen=40), time.time()
    while True:
        try:
            line = q.get(timeout=5)
        except queue.Empty:
            if hb and time.time() - last >= 15:
                last = time.time()
                h = hb()
                if h:
                    PROG.emit(ev='progress', id=h[0], done=int(h[1]), total=int(h[2]), est=h[3] if len(h) > 3 else True)
            continue
        if line is None:
            break
        line = line.rstrip('\r\n')
        say(prefix + line)
        tail.append(line)
        if on_line:
            on_line(line)
    return p.wait(), list(tail)


PROG = Prog(False)


# ------------------------------------------------------------------------------------------------------ window (old dialogs)
def ask_files(a):
    """Fill a.base / a.update / a.keys / a.dump from a small window (tkinter), unless given or --no-gui. Without tkinter
    (the embeddable Python) nothing is asked: the installer goes on with what find_inputs found."""
    if a.dump or a.base or a.no_gui:
        return
    try:
        import tkinter as tk
        from tkinter import filedialog, messagebox
    except ImportError:
        return
    root = tk.Tk(); root.withdraw()
    if messagebox.askyesno('Tomodachi Life: your game files', 'Do you already have a dump FOLDER (with exefs and romfs inside)?\n\n'
                           'Yes = choose that folder.\nNo = choose your package files (.nsp / .xci / .nsz) and your prod.keys.'):
        a.dump = filedialog.askdirectory(title='The dump folder (exefs\\ and romfs\\ inside)') or None
    else:
        pk = [('Switch packages', '*.nsp *.xci *.nsz *.nca'), ('All files', '*.*')]
        a.base = filedialog.askopenfilename(title='The BASE game (v0): .nsp, .xci or .nsz', filetypes=pk) or None
        a.update = filedialog.askopenfilename(title='The UPDATE 1.0.4 (optional: Cancel to play 1.0.0): .nsp or .nsz', filetypes=pk) or None
        if not a.keys and not os.path.exists(os.path.join(os.path.expanduser('~'), '.switch', 'prod.keys')):
            a.keys = filedialog.askopenfilename(title='Your prod.keys', filetypes=[('keys', '*.keys'), ('All files', '*.*')]) or None
    root.destroy()


# ------------------------------------------------------------------------------------------------------ 1. update
def step_update(cx):
    a = cx.a
    if a.no_self_update:
        return 'skip', 'self-update is off (--no-self-update)'
    if a.dry_run:
        return 'dry', 'would ask GitHub for a newer version (scripts/update.py) and start again if files changed'
    lines = []
    rc, _ = stream([sys.executable, os.path.join(HERE, 'update.py')], on_line=lines.append, prefix='   ')
    if rc:
        return 'skip', 'could not update (no internet?); going on with this version'
    if any(l.startswith('Updated ') for l in lines):
        m = next((re.search(r'--from (\w+)', l) for l in lines if '--from' in l and 'setup.py' in l), None)
        cx.restart_from = m.group(1) if m else '-'
        return 'done', 'updated: starting again with the new files' + (' (rebuild from %s)' % m.group(1) if m else '')
    return 'done', 'up to date'


# ------------------------------------------------------------------------------------------------------ 2. precheck
def local_runtime_candidate(a):
    """The runtime package already on disk that find_runtime() would use, without downloading: path or None."""
    if a.no_runtime:
        return None
    if a.runtime:
        return a.runtime if os.path.exists(a.runtime) else None
    url, sha = RUNTIME_RELEASE
    if url and sha:
        p = os.path.join(DEPS, os.path.basename(url))
        return p if os.path.isfile(p) else None
    if os.path.isfile(os.path.join(DEPS, 'runtime', 'manifest.json')):
        return os.path.join(DEPS, 'runtime')
    zips = sorted((os.path.join(DEPS, n) for n in os.listdir(DEPS) if n.startswith('tomodachi-runtime-') and n.endswith('.zip')),
                  key=os.path.getmtime) if os.path.isdir(DEPS) else []
    return zips[-1] if zips else None


def runtime_fit(path):
    """None if the package at path fits this copy of the project (read only: its manifest.json is read, nothing is
    unpacked), else the reason in plain words (setup.runtime_mismatch)."""
    import setup
    m = setup.read_manifest(path)
    if m is None:
        return 'runtime package refused: %s is not a runtime package or cannot be read (a broken download?)' % path
    return setup.runtime_mismatch(m)


def runtime_local(a):
    """Will a prebuilt runtime package be used (no download, no writes)? Then the MSVC Build Tools are not needed.
    A package on disk is checked against this copy (runtime_fit); the pinned Release asset not downloaded yet counts."""
    if a.no_runtime:
        return False
    p = local_runtime_candidate(a)
    if p:
        return runtime_fit(p) is None
    if a.runtime:
        return False
    return bool(RUNTIME_RELEASE[0] and RUNTIME_RELEASE[1])


def nsz_temp_gb(a):
    """Free space the .nsz decompression into <build>\\nsz_cache needs for a while (precheck.nsz_temp_gb); 0 with a dump folder."""
    import precheck
    return 0.0 if a.dump else precheck.nsz_temp_gb([a.base, a.update])


def step_precheck(cx):
    import precheck
    dump = cx.a.dump or (os.path.join(BUILD, 'dump') if dump_ok(os.path.join(BUILD, 'dump')) else None)
    say('   install location: %s%s' % (BUILD, ' (the default)' if build_location.same(BUILD, build_location.default(REPO)) else ''))
    res = precheck.checks(BUILD, need_msvc=not runtime_local(cx.a), dump_dir=dump, temp_gb=0.0 if dump else nsz_temp_gb(cx.a))
    for c in res:
        say('   %-4s %s: %s' % (c['level'].upper(), c['what'], c['msg']))
    fails = ['%s: %s' % (c['what'], c['msg']) for c in res if c['level'] == 'fail']
    warns = [c['what'] for c in res if c['level'] == 'warn']
    if fails:
        if cx.a.dry_run:
            cx.blockers.append('PC check: ' + '; '.join(fails))
            return 'dry', 'would stop: ' + '; '.join(fails)
        raise Stop('this PC cannot run the installer: ' + '; '.join(fails), 'Fix the point above (or use another PC) and start the installer again.')
    return 'done', 'all fine' + (' (warnings: %s)' % ', '.join(warns) if warns else '')


# ------------------------------------------------------------------------------------------------------ 3. packages
def in_private_python():
    deps_py = os.path.normcase(os.path.join(DEPS, 'python')) + os.sep
    return (os.path.normcase(os.path.abspath(sys.prefix)) + os.sep).startswith(deps_py)


def step_packages(cx):
    missing = []
    for p in PKGS:
        mod = {'Pillow': 'PIL', 'pycryptodome': 'Crypto'}.get(p, p)
        try:
            __import__(mod)
        except ImportError:
            missing.append(p)
    if not missing:
        return 'skip', 'all there'
    if cx.a.dry_run:
        return 'dry', 'would install with pip: ' + ' '.join(missing)
    if in_private_python():                  # the launcher makes it, without pip, and makes it again when an import fails (boot::python_ok)
        raise Stop('the installer\'s Python lacks %s' % ' '.join(missing), 'Press TRY AGAIN: the launcher sets it up again. An antivirus may be holding files back.')
    req = os.path.join(REPO, 'requirements.txt')
    cmd = [sys.executable, '-m', 'pip', 'install', '--user']
    cmd += ['-r', req] if os.path.isfile(req) else missing
    say('   installing %s' % ' '.join(missing))
    rc, tail = stream(cmd, prefix='   ')
    if rc:
        raise Stop('pip could not install %s (code %d)' % (' '.join(missing), rc), 'Check the internet connection and start again.')
    return 'done', 'installed ' + ' '.join(missing)


# ------------------------------------------------------------------------------------------------------ 4. inputs
def dump_ok(d):
    """exefs\\main and the game's RomFS folders are there. A dump cut short (a full disk, the window closed) never gets here:
    step_dump unpacks into dump.part and renames it only when the extractor finished."""
    import find_inputs
    return os.path.isfile(os.path.join(d, 'exefs', 'main')) and all(os.path.isdir(os.path.join(d, 'romfs', x)) for x in find_inputs.ROMFS_PROBE)


def has_fonts(folder):
    fonts = os.path.join(folder, 'fonts')
    return os.path.isdir(fonts) and any(n.lower().endswith('.ttf') for n in os.listdir(fonts))


def under(path, folder):
    p, f = os.path.normcase(os.path.abspath(path)), os.path.normcase(os.path.abspath(folder))
    return p == f or p.startswith(f + os.sep)


def step_inputs(cx):
    a, st = cx.a, cx.st
    notes = []
    ready = os.path.join(BUILD, 'dump')
    if a.dump:
        for sub in ('exefs', 'romfs'):
            if not os.path.isdir(os.path.join(a.dump, sub)):
                raise Stop('the dump folder %s has no %s\\ inside' % (a.dump, sub), 'Choose the folder that holds exefs\\ and romfs\\.')
        cx.dump = a.dump
        notes.append('dump folder given with --dump: %s' % a.dump)
    elif dump_ok(ready) and (not (a.base or a.update) or dump_made_from(ready) in (None, made_from_key(a.base, a.update))):
        # 2026-10-20: the window always passes the game files it found, so "already there" never applied and every Resume
        # unpacked the game again; now a complete dump is kept when it was made from the same files (or has no record yet)
        cx.dump = ready
        notes.append('%s is already there (delete that folder to make it again)' % ready)
    if cx.dump and has_fonts(os.path.join(cx.dump, 'sysdata')):
        for n in notes:
            say('   ' + n)
        say('   system fonts: in the dump')
        _record_inputs(cx)
        return 'skip', '; '.join(notes) + '; fonts there, nothing else to look for'

    import find_inputs
    extras = [p for p in (a.base, a.update, a.firmware) if p and os.path.exists(p)]
    r = find_inputs.scan(extras)
    say(find_inputs.summary(r))
    best = r['best']

    def choose(kind, given, items, label):
        if given:
            if os.path.isfile(given):
                return given, 'given'
            if os.path.isdir(given):
                sub = [x['path'] for x in items if under(x['path'], given)]
                if sub:
                    return sub[0], 'found inside the folder you gave'
            cx.blockers.append('%s %s not found' % (label, given))
            return None, ''
        return None, ''

    # firmware (fonts): given, or the best one found
    fw = a.firmware or best.get('firmware')
    cx.firmware = fw
    hit = next((x for x in r['firmware'] if fw and os.path.normcase(os.path.abspath(x['path'])) == os.path.normcase(os.path.abspath(fw))), None)
    cx.firmware_kind = hit['kind'] if hit else None
    if fw:
        notes.append('firmware: %s (%s)' % (fw, 'given' if a.firmware else 'found on this PC'))

    if not cx.dump and not (a.base or a.update) and best.get('dump'):
        cx.dump = best['dump']
        notes.append('dump: %s (a complete extracted dump found on this PC)' % cx.dump)
    if not cx.dump:
        base, why_b = choose('base', a.base, r['base'], 'base game')
        if not base and not a.base:
            base, why_b = best.get('base'), 'best match found on this PC (plain package preferred)'
        upd, why_u = choose('update', a.update, r['updates'], 'update')
        if not upd and not a.update:
            ups = [u for u in r['updates'] if u['build']]
            near = [u for u in ups if base and os.path.dirname(u['path']) == os.path.dirname(base)]
            pick = (near or ups or [None])[0]
            if pick:
                upd, why_u = pick['path'], 'game %s, the newest found%s' % (pick['build'], ' next to the base game' if near else '')
        keys = a.keys or best.get('prod_keys')
        for what, v in (('the base game (.xci / .nsp / .nsz)', base), ('prod.keys', keys)):
            if not v:
                cx.blockers.append('missing: ' + what)
        if base:
            notes.append('base game: %s (%s)' % (base, why_b))
        notes.append('update: %s (%s)' % (upd, why_u) if upd else 'update: none, the game is built as 1.0.0 (the update 1.0.4 is optional)')
        cx.base, cx.update, cx.keys = base, upd, keys
        if keys:
            notes.append('prod.keys: %s (%s)' % (keys, 'given' if a.keys else 'found on this PC'))
            home_keys = os.path.join(os.path.expanduser('~'), '.switch', 'prod.keys')
            if os.path.normcase(os.path.abspath(keys)) != os.path.normcase(home_keys):
                os.environ['SSBU_PROD_KEYS'] = os.path.abspath(keys)   # read by tools/extract/romfs_extract.py; no copy is made
                notes.append('(not in ~/.switch: the tools are told its place with SSBU_PROD_KEYS; no copy is made)')
            tk = best.get('title_keys')
            if tk and 'SSBU_TITLE_KEYS' not in os.environ and os.path.normcase(os.path.abspath(tk)) != os.path.normcase(
                    os.path.join(os.path.expanduser('~'), '.switch', 'title.keys')):
                os.environ['SSBU_TITLE_KEYS'] = os.path.abspath(tk)
    for n in notes:
        say('   ' + n)
    if cx.blockers and not a.dry_run:
        # the old dialogs: only now, when something is still missing, and only with a window available
        if not a.no_gui:
            ask_files(a)
            if a.dump or a.base or a.update:
                cx.blockers[:] = []
                a.firmware = a.firmware or cx.firmware
                return step_inputs(cx)
        raise Stop('missing: ' + '; '.join(cx.blockers), 'Give them with --base / --update / --keys, or put the game files in Downloads / Documents and your '
                   'prod.keys in %USERPROFILE%\\.switch, then start again.')
    _record_inputs(cx)
    return 'done', '; '.join(notes) if notes else 'nothing found'


def _record_inputs(cx):
    cx.st.d['inputs'] = {'dump': cx.dump, 'base': cx.base, 'update': cx.update, 'firmware': cx.firmware,
                         'keys_file': cx.keys}          # paths only, never key values
    cx.st.save()


# ------------------------------------------------------------------------------------------------------ 5. dump
def ensure_fonts(cx, dump):
    """The system fonts: the dump's, else your firmware's, else the open-source ones of runtime\\fonts_open (the runtime stops,
    code 7, only without all three). -> extra setup.py arguments (--sysdata), [] if the dump has them or the open ones are used."""
    if has_fonts(os.path.join(dump, 'sysdata')):
        return []
    mine = os.path.join(BUILD, 'sysdata')
    if has_fonts(mine):
        return ['--sysdata', mine]
    fw = cx.firmware
    if fw and os.path.isdir(fw):
        out = os.path.join(mine, 'fonts')
        if cx.firmware_kind == 'fonts' or (not cx.firmware_kind and any(n.lower().endswith(('.ttf', '.bfttf', '.otf')) for n in os.listdir(fw))):
            os.makedirs(out, exist_ok=True)
            for n in os.listdir(fw):
                if n.lower().endswith(('.ttf', '.otf')):
                    shutil.copyfile(os.path.join(fw, n), os.path.join(out, n))
        else:
            say('   system fonts: extracting from the firmware %s' % fw)
            rc, _ = stream([sys.executable, os.path.join(REPO, 'tools', 'extract', 'fw_fonts.py'), fw, out], prefix='   ')
            if rc:                            # optional: the open-source fonts below are used instead (never a stop)
                say('   note: the system fonts could not be extracted from %s (code %d): the open-source fonts are used' % (fw, rc))
        if has_fonts(mine):
            return ['--sysdata', mine]
    # no firmware fonts: the open-source replacements the repository ships (runtime\fonts_open, README.md there: Open Sans,
    # Source Sans Pro, the yuzu project's icon font); setup.py stages them beside the exe, where nnpl.c looks for them
    shipped = os.path.join(REPO, 'runtime', 'fonts_open')
    if os.path.isdir(shipped) and sum(n.lower().endswith('.ttf') for n in os.listdir(shipped)) >= 7:
        say('   system fonts: no firmware given, the open-source fonts of runtime\\fonts_open are used (no Chinese, Japanese '
            'or Korean characters; a firmware folder adds your console\'s own)')
        return []
    raise Stop('no system fonts: neither a firmware nor the open-source fonts of runtime\\fonts_open (the game stops at start without)',
               'Restore runtime\\fonts_open (scripts/update.py, or tools/extract/open_fonts.py), or give your firmware folder (--firmware).')


def made_from_key(base, update):
    """The game files a dump comes from: names and sizes (no paths: the same files moved elsewhere still match)."""
    def one(p):
        try:
            return '%s:%d' % (os.path.basename(p).lower(), os.path.getsize(p) if os.path.isfile(p) else -1)
        except (OSError, TypeError):
            return str(p)
    return '%s|%s' % (one(base), one(update))


def dump_made_from(dump):
    try:
        with open(os.path.join(dump, 'made_from.json'), encoding='utf-8') as f:
            return json.load(f).get('key')
    except (OSError, ValueError):
        return None


def step_dump(cx):
    a = cx.a
    if cx.dump:
        if a.dry_run:
            fonts = has_fonts(os.path.join(cx.dump, 'sysdata'))
            return 'dry', 'would use the dump %s%s' % (cx.dump, '' if fonts else (' and add the system fonts from the firmware' if cx.firmware
                                                                       else ' with the open-source fonts of runtime\\fonts_open'))
        cx.setup_args += ensure_fonts(cx, cx.dump)
        return 'skip', 'using the dump %s' % cx.dump
    if not cx.base:
        return 'dry', 'would stop: no base game'
    ext = os.path.join(REPO, 'tools', 'extract')
    out, part = os.path.join(BUILD, 'dump'), os.path.join(BUILD, 'dump.part')
    pkgs = [p for p in (cx.base, cx.update) if p]
    if a.dry_run:
        return 'dry', 'would make %s (about 7 GB, a few minutes) from %s with dump_folder.py%s' % (
            out, ' + '.join(os.path.basename(p) for p in pkgs), '; a .nsz is first decompressed into %s (removed after)'
            % os.path.join(BUILD, 'nsz_cache') if any(p.lower().endswith('.nsz') for p in pkgs) else '')
    os.makedirs(BUILD, exist_ok=True)
    env = dict(os.environ)
    own_cache = 'SSBU_TMP' not in env
    if own_cache:
        env['SSBU_TMP'] = os.path.join(BUILD, 'nsz_cache')      # romfs_extract.py unpacks an .nsz here (our own folder, removed after)
    say('   making %s (about 7 GB, a few minutes)' % out)
    cmd = [sys.executable, os.path.join(ext, 'dump_folder.py')] + pkgs + [part]
    if cx.firmware and os.path.isdir(cx.firmware) and cx.firmware_kind == 'nca-folder':
        cmd += ['--firmware', cx.firmware]
    rc, tail = stream(cmd, env=env, hb=lambda: ('dump', time.time() - PROG.t0.get('dump', time.time()), EST['dump']), prefix='   ')
    if rc:
        why = [l.strip() for l in (tail or []) if l.strip()][-3:]      # the extractor's own last lines: the real reason
        raise Stop('the game files could not be unpacked (code %d): %s' % (rc, ' | '.join(why)),
                   'Check that prod.keys belongs to the same console as the game files, and that base and update are the same game.')
    if os.path.isdir(out) and not dump_ok(out):
        os.replace(out, out + '.old_%d' % int(time.time()))      # an unfinished dump of an older run: kept aside, not deleted
    rebuilt = False
    if os.path.isdir(out) and dump_ok(out):
        # a complete dump made from OTHER game files (the player added the update, or chose other files): the installer
        # made it, the new one replaces it, and the build is made again from the conversion on
        say('   the game files changed (%s): the old unpacked game is replaced and the game is built again' % os.path.basename(pkgs[-1]))
        shutil.rmtree(out, ignore_errors=True)
        rebuilt = True
    if not os.path.isdir(out):
        os.replace(part, out)
    if rebuilt:
        cx.setup_args += ['--from', 'convert']
    try:                                                         # what this dump was made from (Resume keeps it next time)
        with open(os.path.join(out, 'made_from.json'), 'w', encoding='utf-8') as f:
            json.dump({'key': made_from_key(cx.base, cx.update)}, f)
    except OSError:
        pass
    if own_cache:
        shutil.rmtree(env['SSBU_TMP'], ignore_errors=True)
    cx.dump = out
    cx.setup_args += ensure_fonts(cx, out)
    return 'done', '%s made' % out


# ------------------------------------------------------------------------------------------------------ 6. tools
# the two files of a yuzu-family source tree that tools/lifter/gen_sdktest.py reads (default Miis for the self-test), from
# suyu-emu/suyu-v0.0.4 commit c0dd2ff5 (MIT / GPL-3.0), mirrored by name in this project's files repository (MIRROR); hashes of the
# files as GitHub served them (LF line ends)
SUYU_FILES = {'src/core/hle/service/mii/types/raw_data.cpp': '0f5b10dcde4038024fffd6079d75cd4bad8d72486efcf1ffa06e6c779803c486',
              'src/core/hle/service/mii/mii_types.h': 'cf2e0c990b881c238f5b7fd823f33fe2c0a8d0846cdbf012ec6f702b1647f3e1'}


def cert_failure(e):
    """True when e is Python's TLS check refusing the server's certificate (not a network error)."""
    reason = getattr(e, 'reason', e)
    return isinstance(reason, ssl.SSLError) or 'CERTIFICATE_VERIFY_FAILED' in str(e)


def download(url, dst):
    """url into the file dst. Python checks certificates against the roots Windows already has; a PC that never needed the
    root GitHub's download host uses had not fetched it yet, and Python refused every download (issue #13, "unable to get
    local issuer certificate"). Windows' own curl.exe checks with Windows, which fetches a missing root as browsers do, so it
    gets a second try. Every download here is checked against its SHA-256 afterwards either way."""
    try:
        urllib.request.urlretrieve(url, dst)
        return
    except OSError as e:
        if not cert_failure(e):
            raise
        say('   Python could not check the server\'s certificate (%s): downloading with Windows\' curl instead' % getattr(e, 'reason', e))
    curl = os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'curl.exe')
    if not os.path.isfile(curl):
        curl = shutil.which('curl') or curl
    r = subprocess.run([curl, '-L', '--fail', '--silent', '--show-error', '--retry', '3', '-o', dst, url])
    if r.returncode:
        raise OSError('curl could not download %s either (exit %d)' % (url, r.returncode))


def fetch_suyu():
    dst = os.path.join(DEPS, 'suyu')
    for rel, sha in SUYU_FILES.items():
        out = os.path.join(dst, *rel.split('/'))
        if os.path.exists(out):
            continue
        os.makedirs(os.path.dirname(out), exist_ok=True)
        download(MIRROR + rel.rsplit('/', 1)[1], out + '.part')
        data = open(out + '.part', 'rb').read()
        os.remove(out + '.part')
        if hashlib.sha256(data).hexdigest() != sha:
            raise Stop('suyu %s: the download does not match its SHA-256: refused' % rel)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        open(out, 'wb').write(data)
    return dst


def run_elevated(exe, params):
    """exe with params after Windows' administrator prompt (ShellExecuteEx "runas", as Start-Process -Verb RunAs did through
    PowerShell); waits for it. -> its exit code, None when the prompt was answered No."""
    import ctypes
    from ctypes import wintypes

    class SEI(ctypes.Structure):          # SHELLEXECUTEINFOW
        _fields_ = [('cbSize', wintypes.DWORD), ('fMask', wintypes.ULONG), ('hwnd', wintypes.HWND), ('lpVerb', wintypes.LPCWSTR),
                    ('lpFile', wintypes.LPCWSTR), ('lpParameters', wintypes.LPCWSTR), ('lpDirectory', wintypes.LPCWSTR),
                    ('nShow', ctypes.c_int), ('hInstApp', wintypes.HINSTANCE), ('lpIDList', ctypes.c_void_p), ('lpClass', wintypes.LPCWSTR),
                    ('hkeyClass', wintypes.HKEY), ('dwHotKey', wintypes.DWORD), ('hIconOrMonitor', wintypes.HANDLE), ('hProcess', wintypes.HANDLE)]
    sei = SEI(cbSize=ctypes.sizeof(SEI), fMask=0x40, lpVerb='runas', lpFile=exe, lpParameters=params, nShow=1)      # SEE_MASK_NOCLOSEPROCESS
    if not ctypes.windll.shell32.ShellExecuteExW(ctypes.byref(sei)) or not sei.hProcess:
        return None
    k32 = ctypes.windll.kernel32
    k32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    k32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    k32.CloseHandle.argtypes = [wintypes.HANDLE]
    k32.WaitForSingleObject(sei.hProcess, 0xFFFFFFFF)
    code = wintypes.DWORD(1)
    k32.GetExitCodeProcess(sei.hProcess, ctypes.byref(code))
    k32.CloseHandle(sei.hProcess)
    return code.value


def fetch(name):
    url, sha, folder, probe = TOOLS[name]
    dst = os.path.join(DEPS, folder)
    if os.path.exists(os.path.join(dst, probe)):
        return dst
    if not sha:
        raise Stop('%s is missing and its download is not verified yet (no SHA-256 listed in scripts/install.py)' % name,
                   'Install it by hand (BUILDING.md) or give its folder to setup.py.')
    os.makedirs(DEPS, exist_ok=True)
    arc = os.path.join(DEPS, os.path.basename(url))
    say('   downloading %s' % url)
    download(url, arc)
    h = hashlib.sha256(open(arc, 'rb').read()).hexdigest()
    if h != sha:
        os.remove(arc)
        raise Stop('%s: the download does not match its SHA-256 (got %s): refused' % (name, h))
    if arc.endswith('.zip'):
        if os.path.isdir(dst):                  # an unfinished folder of an earlier try (no probe file in it): replaced
            shutil.rmtree(dst, ignore_errors=True)
        tmp = dst + '.tmp'
        with zipfile.ZipFile(arc) as z:
            z.extractall(tmp)
        inner = [os.path.join(tmp, d) for d in os.listdir(tmp)]
        shutil.move(inner[0] if len(inner) == 1 and os.path.isdir(inner[0]) else tmp, dst)
        shutil.rmtree(tmp, ignore_errors=True)
    else:                                       # the LLVM installer: silent, into deps\llvm
        try:
            subprocess.run([arc, '/S', '/D=' + dst], check=True)
        except OSError as e:                    # 740: it asks for administrator rights; Windows shows its Yes/No prompt
            if getattr(e, 'winerror', None) != 740:
                raise
            say('   the LLVM installer asks for administrator rights: answer Yes in the Windows prompt')
            run_elevated(arc, '/S /D=' + dst)      # (NSIS: /D= last and unquoted)
        if not os.path.exists(os.path.join(dst, 'bin', 'clang.exe')):
            raise Stop('LLVM was not installed into %s (was the Windows prompt answered No?)' % dst, 'Start the installer again.')
    os.remove(arc)
    return dst


def find_runtime(a, why=None):
    """The runtime package to use: --runtime, the pinned GitHub Release asset (downloaded into deps/ and checked), or a
    tomodachi-runtime-*.zip / runtime folder already in deps/. None: build everything here (why, a list, gets the reason)."""
    why = why if why is not None else []
    if a.no_runtime:
        why.append('"Use the prebuilt runtime" is off (--no-runtime)')
        return None
    if a.runtime:
        if not os.path.exists(a.runtime):
            raise Stop('--runtime %s: not found' % a.runtime)
        return os.path.abspath(a.runtime)
    url, sha = RUNTIME_RELEASE
    if url and sha:
        dst = os.path.join(DEPS, os.path.basename(url))
        if not (os.path.isfile(dst) and hashlib.sha256(open(dst, 'rb').read()).hexdigest() == sha):
            os.makedirs(DEPS, exist_ok=True)
            say('   downloading the prebuilt runtime %s' % url)
            try:
                download(url, dst + '.part')
            except OSError as e:
                say('   the prebuilt runtime could not be downloaded (%s): everything is built here instead' % e)
                why.append('the prebuilt runtime could not be downloaded from GitHub (%s): check the internet connection '
                           '(or a firewall / antivirus blocking github.com) and press Resume' % e)
                return None
            got = hashlib.sha256(open(dst + '.part', 'rb').read()).hexdigest()
            if got != sha:
                os.remove(dst + '.part')
                raise Stop('the prebuilt runtime does not match its SHA-256 (got %s): refused' % got)
            os.replace(dst + '.part', dst)
        return dst
    if os.path.isfile(os.path.join(DEPS, 'runtime', 'manifest.json')):
        return os.path.join(DEPS, 'runtime')
    zips = sorted((os.path.join(DEPS, n) for n in os.listdir(DEPS) if n.startswith('tomodachi-runtime-') and n.endswith('.zip')),
                  key=os.path.getmtime) if os.path.isdir(DEPS) else []
    if not zips:
        why.append('no prebuilt runtime is published for this version of the project')
    return zips[-1] if zips else None


def probe_failure(stdout):
    """setup.py's "FAILED at step <s>: <message>" line -> the message ('' when there is none). Only that line counts:
    before 2026-10-20 any line with "runtime package" in it was taken for a refusal, and setup.py prints
    "  runtime package: <folder> (commit ...)" when it ACCEPTS the package, so a PC without LLVM yet (every new tester:
    LLVM is fetched only after this probe) was told the runtime cannot be used and then that MSVC is missing."""
    for line in stdout.splitlines():
        m = re.match(r'FAILED at step \w+: (.*)', line.strip())
        if m:
            return m.group(1).strip()
    return ''


def probe_runtime(dump, runtime, extra):
    """setup.py --only deps with the package (it unpacks and checks it in <build>\\runtime) -> (exit code, failure message)."""
    probe = [sys.executable, os.path.join(HERE, 'setup.py'), '--only', 'deps', '--build', BUILD, '--dump', dump, '--runtime', runtime] + extra
    r = subprocess.run(probe, capture_output=True, text=True, errors='replace')
    return r.returncode, probe_failure(r.stdout) or (r.stdout + r.stderr).strip()[-300:]


def msvc_found():
    vsw = os.path.join(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'), 'Microsoft Visual Studio', 'Installer', 'vswhere.exe')
    return bool(os.path.exists(vsw) and subprocess.run([vsw, '-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                                                        '-property', 'installationPath'], capture_output=True, text=True, errors='replace').stdout.strip())


def msvc_stop(why):
    """The stop when the exe has to be built here and the MSVC Build Tools are missing: first WHY the prebuilt runtime is
    not used (that is what the user can usually fix), then how to get the Build Tools with a visible progress window."""
    lead = ('The prebuilt runtime cannot be used: %s. ' % why.rstrip('. ')) if why else ''
    return Stop(lead + 'Without it the whole game, its exe too, is built on this PC, and that needs the Microsoft C++ Build Tools, '
                'which are missing.',
                'Usually the fix is the point above (then press Resume). Otherwise install the Build Tools (free, about 3 GB on C:, '
                '5-15 minutes; Windows asks for administrator rights; Microsoft\'s installer shows its progress): press "Install '
                'Build Tools" below, or run this in a Command Prompt, then press Resume:  ' + MSVC_WINGET, action='msvc')


def tools_args(dump, runtime=None, extra=(), why=''):
    extra = list(extra)
    if runtime:                                  # only LLVM is needed: no MSVC, no SDL3 / Vulkan-Headers / suyu
        rc, fail = probe_runtime(dump, runtime, extra)
        llvm = []
        if rc and 'LLVM/clang' in fail:          # no LLVM yet: fetch it (into deps\llvm) and ask again
            llvm = ['--llvm', fetch('llvm')]
            rc, fail = probe_runtime(dump, runtime, extra + llvm)
        if rc == 0:
            say('   LLVM there; prebuilt runtime %s (no MSVC Build Tools needed)' % os.path.basename(runtime))
            return ['--runtime', runtime] + llvm
        if 'runtime package refused' not in fail:
            # a problem that has nothing to do with the package (the dump, a Python package): say it now, not "MSVC"
            raise Stop('the tools check stopped: %s' % fail[:400], 'Fix the point above, then press Resume (see the logs in %s).'
                       % os.path.join(BUILD, 'logs'))
        why = fail.split('runtime package refused:', 1)[1].strip()
        say('   the prebuilt runtime cannot be used: %s' % why[:600])
        say('   so everything, the exe too, would be built here (needs the MSVC Build Tools)')
    if not msvc_found():
        raise msvc_stop(why)
    extra = []                                  # Dear ImGui is optional (the F1 menu): setup.py --only deps passes without it
    try:
        extra = ['--imgui', fetch('imgui')]
    except (Exception, SystemExit) as e:        # fetch() exits on a bad hash; only the menu is lost
        say('   WARNING: Dear ImGui not available (%s): the settings menu (F1) is left out of the build' % e)
    if subprocess.run([sys.executable, os.path.join(HERE, 'setup.py'), '--only', 'deps', '--build', BUILD, '--dump', dump],
                      capture_output=True).returncode == 0:
        say('   all found where setup.py looks')
        return extra
    args = []
    for name, flag in (('llvm', '--llvm'), ('sdl3', '--sdl3'), ('vulkan-headers', '--vulkan-headers')):
        args += [flag, fetch(name)]
    args += ['--suyu', fetch_suyu()]
    say('   all there')
    return args + extra


def step_tools(cx):
    a = cx.a
    if a.dry_run:
        p = local_runtime_candidate(a)
        why = runtime_fit(p) if p else None
        if why:
            msg = 'the prebuilt runtime %s cannot be used: %s' % (os.path.basename(p), why.split(':', 1)[1].strip())
            if msvc_found():
                return 'dry', msg + '; the MSVC Build Tools are there, so everything would be built here'
            cx.blockers.append(msg)
            return 'dry', msg + '; and the MSVC Build Tools are missing: it would stop here'
        if runtime_local(a):
            return 'dry', ('would check LLVM/clang 9 (download it into deps\\llvm, SHA-256 checked, if missing); prebuilt runtime %s: '
                           'no MSVC needed' % ('%s fits this copy' % os.path.basename(p) if p else 'to download'))
        return 'dry', ('would check LLVM, SDL3, Vulkan-Headers, suyu (download what is missing, SHA-256 checked) and the MSVC '
                       'Build Tools (%s)' % ('found' if msvc_found() else 'MISSING: it would stop here'))
    why = []
    rt = find_runtime(a, why)
    cx.setup_args += tools_args(cx.dump, rt, cx.setup_args, why[0] if why else '')
    return 'done', 'tools ready'


# ------------------------------------------------------------------------------------------------------ 7. media
def step_media(cx):
    a = cx.a
    import fetch_media
    dll = os.path.join(DEPS, 'opus', 'libopus-0.dll')
    if a.dry_run:
        msg = 'libopus is already in deps\\opus' if os.path.isfile(dll) else 'would download libopus (music) into deps\\opus, SHA-256 checked'
    else:
        got = fetch_media.fetch_opus(DEPS)
        msg = 'libopus ready' if got else 'no music (libopus could not be fetched; the game runs silent)'
    # FFmpeg: the movies' fallback where Windows cannot play WebM (codecs missing or broken, Windows N); always fetched,
    # since the Store check below cannot tell a broken copy of the codecs from a working one
    if a.dry_run:
        ff = fetch_media.ffmpeg_ready(DEPS)
        msg += '; FFmpeg is already in deps\\ffmpeg' if ff else '; would download FFmpeg (movie fallback, 3 MB) into deps\\ffmpeg, SHA-256 checked'
    else:
        ff = fetch_media.fetch_ffmpeg(DEPS)
        msg += '; FFmpeg ready (movie fallback)' if ff else '; no FFmpeg (movies only with Windows\' own codecs)'
    codecs = fetch_media.check_codecs()
    missing = [k for k, v in codecs.items() if v is not True]
    if missing:
        msg += '; Store codecs missing or unknown: ' + ', '.join(missing) + (' (FFmpeg plays the movies)' if ff else ' (Store links printed above)')
    return ('dry' if a.dry_run else 'done'), msg


# ------------------------------------------------------------------------------------------------------ 8. build
def step_build(cx, P):
    """setup.py, its "[k/11] name ..." lines forwarded as step events with the ids of setup.py's steps."""
    a, st = cx.a, cx.st
    if a.dry_run:
        done = {}
        try:
            with open(os.path.join(BUILD, 'state.json')) as f:
                done = json.load(f).get('done', {})
        except (OSError, ValueError):
            pass
        for s in SETUP_STEPS:
            P.start(s)
            if s in done and s not in ('deps', 'stage'):
                P.end(s, 'skip', 'done earlier (build\\state.json)')
            else:
                P.end(s, 'dry', 'would run scripts/setup.py step %s' % s)
        return
    cmd = [sys.executable, os.path.join(HERE, 'setup.py'), '--build', BUILD, '--dump', cx.dump] + cx.setup_args
    if a.jobs:
        cmd += ['--jobs', str(a.jobs)]
    if cx.rebuild_from and cx.rebuild_from in SETUP_STEPS:
        cmd += ['--from', cx.rebuild_from]
        say('   after the update, setup.py repeats from step %s (the compilers redo only what changed)' % cx.rebuild_from)
    say('   scripts/setup.py (about 30 minutes to an hour the first time; it can be stopped and started again)')
    cur = {'id': None}
    scale = 12.0 / cx.jobs

    def est(s):
        return int(EST[s] * scale) if s == 'compile' else EST[s]

    def on_line(line):
        m = re.match(r'\[(\d+)/(\d+)\] (\w+)(.*)$', line)
        if not m or m.group(3) not in SETUP_STEPS:
            m2 = re.match(r'FAILED at step (\w+): (.*)', line)
            if m2 and cur['id']:
                P.end(cur['id'], 'fail', m2.group(2)[:300])
                cur['id'] = None
            return
        s, rest = m.group(3), m.group(4).strip()
        if rest == '...':
            if cur['id'] and cur['id'] != s:
                P.end(cur['id'], 'done')
            cur['id'] = s
            P.start(s)
        elif rest.startswith(': ok'):
            P.t0.setdefault(s, time.time())
            P.end(s, 'done', '', secs=secs_of(rest[4:]) or None)
            st.done(s, secs_of(rest[4:]), '')
            cur['id'] = None
        elif 'skipped' in rest:
            P.start(s)
            P.end(s, 'skip', rest.lstrip(': '))
        elif 'would run' in rest:
            P.start(s)
            P.end(s, 'dry', 'would run')

    def compiled(s):
        """(done, total) files of this run of the step from its log (build.py prints "  N/M   Ts elapsed" after the line
        "sources: S   to compile: M"), or None. Before, the window showed seconds against the estimate (2835 / 2660)."""
        if s != 'compile':
            return None
        try:
            with open(os.path.join(BUILD, 'logs', 'compile.log'), 'rb') as f:
                f.seek(0, 2)
                f.seek(max(0, f.tell() - 262144))
                t = f.read().decode('utf-8', 'replace')
        except OSError:
            return None
        i = t.rfind('sources: ')
        m = re.match(r'sources: \d+\s+to compile: (\d+)', t[i:]) if i >= 0 else None
        if not m or not int(m.group(1)):
            return None
        n = re.findall(r'^\s+(\d+)/\d+\s+\d+s elapsed', t[i:], re.M)
        return (int(n[-1]) if n else 0, int(m.group(1)))

    def hb():
        s = cur['id']
        if s not in ('compile', 'lift', 'link', 'prepare'):
            return None
        c = compiled(s)
        if c:
            return (s, c[0], c[1], False)
        e = est(s)
        return (s, min(time.time() - P.t0.get(s, time.time()), 0.95 * e), e)      # seconds: never past the estimate
    rc, tail = stream(cmd, on_line=on_line, hb=hb, prefix='   ')
    if rc:
        if cur['id']:
            P.end(cur['id'], 'fail', 'see build\\logs')
        raise Stop('the build stopped (code %d): %s' % (rc, next((l for l in reversed(tail) if l.strip()), '')[:300]),
                   'See build\\logs, then start the installer again: it goes on from there.')
    if cur['id']:
        P.end(cur['id'], 'done')


# ------------------------------------------------------------------------------------------------------ 9. shortcuts
def step_shortcuts(cx):
    import shortcuts
    res = shortcuts.create_all(REPO, dry=cx.a.dry_run)
    for l in res:
        say('   ' + l)
    bad = [l for l in res if l.startswith('fail')]
    if bad:
        return 'skip', 'shortcuts not made: ' + bad[0]       # not worth stopping a finished install
    if cx.a.dry_run:
        return 'dry', 'would make %d shortcut(s)' % len(res)
    return 'done', '%d shortcut(s): %s' % (len(res), ', '.join(l.split(' ', 1)[0] for l in res))


# ------------------------------------------------------------------------------------------------------ main
class Ctx:
    def __init__(self, a, st):
        self.a, self.st = a, st
        self.dump = self.base = self.update = self.keys = self.firmware = self.firmware_kind = None
        self.setup_args, self.blockers = [], []
        self.restart_from = None
        self.rebuild_from = a.reexec if a.reexec and a.reexec != '-' else None
        self.jobs = a.jobs or default_jobs()


def default_jobs():
    """Compiler jobs when none was given: precheck's rule (about 1.5 GB of RAM per job, at most the logical CPUs), at most
    24. Only the CPU count made a 32-thread PC run 24 clang processes at once, and big files then failed for lack of memory."""
    try:
        import precheck
        return max(1, min(24, precheck.suggested_jobs(precheck._ram_gb())))
    except Exception:                            # noqa: BLE001
        return min(8, os.cpu_count() or 4)


def make_plan(cx):
    a = cx.a
    ids = ['update', 'precheck', 'packages', 'inputs', 'dump', 'tools', 'media']
    if not a.prepare_only:
        ids += SETUP_STEPS
        if not a.no_shortcuts:
            ids.append('shortcuts')
    plan = []
    for s in ids:
        e = EST[s]
        if s == 'compile':
            e = int(e * 12.0 / cx.jobs)
        if s == 'dump' and (a.dump or dump_ok(os.path.join(BUILD, 'dump'))):
            e = 5
        if s == 'update' and a.no_self_update:
            e = 0
        plan.append({'id': s, 'title': TITLES[s], 'est_s': e})
    return plan


def parse(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0], allow_abbrev=False)
    ap.add_argument('--no-gui', action='store_true', help='never open a dialog (the installer window passes this)')
    ap.add_argument('--progress-json', action='store_true', help='also print "##ssbu {json}" progress lines')
    ap.add_argument('--dry-run', action='store_true', help='say what each step would do ("would ..."), change nothing')
    ap.add_argument('--base', help='the base game: .xci / .nsp / .nsz / .nca, or a folder holding it')
    ap.add_argument('--update', help='the update 1.0.4 (optional): .nsp / .nsz / .nca, or a folder holding it')
    ap.add_argument('--keys', help='your prod.keys (used where it is, never copied)')
    ap.add_argument('--dump', help='a dump folder you already have (exefs\\ and romfs\\ inside)')
    ap.add_argument('--firmware', help='your firmware folder (.nca files) or the extracted system fonts')
    ap.add_argument('--build-dir', help='install location: the folder for the unpacked game, the build, the logs, the saves and the '
                                        '.nsz decompression (about 15 GB; default <repo>\\build). Remembered in build_location.txt, '
                                        'so Resume, setup.py, run.py, the launcher and the shortcuts use it too')
    ap.add_argument('--jobs', type=int, help='parallel compiler jobs')
    ap.add_argument('--prepare-only', action='store_true', help='stop before the build (steps 1-6 only)')
    ap.add_argument('--runtime', help='prebuilt runtime package (zip or folder) to use instead of the one of the GitHub Release')
    ap.add_argument('--no-runtime', action='store_true', help='build everything here, the exe too (needs the MSVC Build Tools)')
    ap.add_argument('--no-shortcuts', action='store_true', help='do not make the desktop / Start menu shortcuts')
    ap.add_argument('--no-self-update', action='store_true', help='do not look for a newer version of this program first')
    ap.add_argument('--reexec', help=argparse.SUPPRESS)       # internal: started again after a self-update (earliest setup step to repeat)
    return ap.parse_args(argv)


def run_step(cx, P, sid, fn):
    P.start(sid)
    t0 = time.time()
    say('== %s' % TITLES[sid])
    try:
        state, msg = fn(cx)
    except Stop:
        P.end(sid, 'fail', sys.exc_info()[1].msg[:300])
        raise
    except Exception as e:                       # a bug or a missing module: reported like a stop
        P.end(sid, 'fail', '%s: %s' % (type(e).__name__, e))
        raise Stop('%s failed: %s: %s' % (sid, type(e).__name__, e), 'See build\\logs; start the installer again, or report this message.')
    secs = time.time() - t0
    say('   -> %s: %s' % (state, msg))
    P.end(sid, state, msg[:300], secs)
    if state == 'done' and not cx.a.dry_run:
        cx.st.done(sid, secs, msg)


def main(argv=None):
    global PROG
    argv = list(sys.argv[1:] if argv is None else argv)
    a = parse(argv)
    PROG = P = Prog(a.progress_json)
    if a.build_dir:                              # the install location chosen in the window: used now and remembered
        set_build(os.path.expandvars(a.build_dir.strip().strip('"')))
        if not a.dry_run:
            try:
                os.makedirs(BUILD, exist_ok=True)
                build_location.save(BUILD, REPO)
            except OSError as e:
                say('STOPPED: the install location %s cannot be used (%s)' % (BUILD, e))
                P.emit(ev='error', msg='the install location %s cannot be used: %s' % (BUILD, e),
                       hint='Choose another folder (one you can write to, on a drive with enough free space) and start again.')
                return 1
    st = State(a.dry_run)
    cx = Ctx(a, st)
    try:
        if not a.reexec:
            P.emit(ev='plan', steps=make_plan(cx))
            run_step(cx, P, 'update', step_update)
            if cx.restart_from:                  # new files: start again once with them
                rc = subprocess.run([sys.executable, os.path.abspath(__file__)] + argv + ['--no-self-update', '--reexec', cx.restart_from]).returncode
                return rc
        run_step(cx, P, 'precheck', step_precheck)
        run_step(cx, P, 'packages', step_packages)
        run_step(cx, P, 'inputs', step_inputs)
        run_step(cx, P, 'dump', step_dump)
        run_step(cx, P, 'tools', step_tools)
        run_step(cx, P, 'media', step_media)
        if a.prepare_only:
            say('prepared: the build itself is not started (--prepare-only)')
            P.emit(ev='done', ok=not cx.blockers, msg='prepared; the build is not started (--prepare-only)')
            return 0
        say('== Build the game')
        step_build(cx, P)
        if not a.no_shortcuts:
            run_step(cx, P, 'shortcuts', step_shortcuts)
    except Stop as e:
        say('STOPPED: %s' % e.msg)
        if e.hint:
            say(e.hint)
        P.emit(ev='error', msg=e.msg, hint=e.hint, action=e.action)
        return 1
    if a.dry_run:
        msg = 'dry run finished, nothing was changed' + ('; it would stop: ' + '; '.join(cx.blockers) if cx.blockers else '')
        say(msg)
        P.emit(ev='done', ok=not cx.blockers, msg=msg)
        return 0
    try:                                         # the release installed: the launcher updates an older one before PLAY (installed_version)
        with open(os.path.join(REPO, 'version.txt')) as f:
            cx.st.d['version'] = f.read().strip()
        cx.st.save()
    except OSError:
        pass
    say('Done: start "Tomodachi Life.exe" in this folder (or the Tomodachi Life shortcut)')
    P.emit(ev='done', ok=True, msg='Ready: start "Tomodachi Life.exe"')
    return 0


if __name__ == '__main__':
    sys.exit(main())
