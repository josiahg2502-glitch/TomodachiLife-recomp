#!/usr/bin/env python3
"""Music and movies out of the box: the two pieces the game needs that are not part of this repository.

    python scripts/fetch_media.py [--deps <folder>] [--force] [--check-codecs]

* Music: the game's Opus streams are decoded by libopus, which runtime/nnaudio.c loads at run time (opus.dll or
  libopus-0.dll, next to the exe or named by SSBU_OPUS_DLL). fetch_opus() downloads a pinned prebuilt x64 libopus
  (MSYS2's ucrt64 package of Opus 1.6.1, BSD licence; a copy in this project's files repository), checks the package's SHA-256, unpacks
  libopus-0.dll, checks the DLL's SHA-256, checks that it imports only Windows system DLLs (the Universal CRT forwarders
  and KERNEL32: no libgcc / libwinpthread to ship) and that it exports the three functions nnaudio.c needs, and puts it
  in <deps>/opus/ (the folder scripts/setup.py already looks in). Download the .zst needs the `zstandard` module (pip
  installs it for your user if missing; Windows' own tar.exe is the fallback).
* Movies: the menus' and the game's movies are VP9 in WebM; Windows decodes them with two free Microsoft Store
  extensions. check_codecs() reports whether they are installed. This program cannot install them: the Store asks you
  to (the links are printed). Where they are missing or broken (and on Windows N), the runtime plays the movies with
  FFmpeg instead (runtime/mf_ffmpeg.c): fetch_ffmpeg() downloads that pinned LGPL FFmpeg build (a GitHub Release asset
  of this project, made by scripts/ffmpeg/), checks the package's and each DLL's SHA-256 and the DLLs' imports, and
  puts it in <deps>/ffmpeg/ (bin/ the DLLs setup.py copies beside the exe, include/ the headers build.py compiles with).

Standard library only, except `zstandard` for the .zst (see above). The download idea (a pinned prebuilt audio library,
verified before use) follows WindowSmash's tools/install_windowsmash.ps1 by SNO2207; this is a separate implementation.
"""
import argparse
import hashlib
import io
import os
import shutil
import ssl
import struct
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

REPO_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# MSYS2's package (repo.msys2.org/mingw/ucrt64 drops old versions) and the FFmpeg zip of ssbu-recomp's release, both kept
# in this project's files repository, byte for byte (the SHA-256 below)
MIRROR = 'https://github.com/josiahg2502-glitch/TomodachiLife-recomp-files/releases/download/installer-tools/'
OPUS_PKG = 'mingw-w64-ucrt-x86_64-opus-1.6.1-2-any.pkg.tar.zst'
OPUS_URL = MIRROR + OPUS_PKG
OPUS_PKG_SHA256 = 'bb114bcf26225f50f874497ebfd6b8224fff2a20bda3f8ee45cd07ce5f02657b'
OPUS_DLL_MEMBER = 'ucrt64/bin/libopus-0.dll'
OPUS_DLL_SHA256 = 'e747edad6bf02cf5cdcce417b95be3b7ba108f93df1b024778be5d73fa6dae39'
OPUS_LICENSE_MEMBER = 'ucrt64/share/licenses/opus/COPYING'
OPUS_EXPORTS = ('opus_decoder_get_size', 'opus_decoder_init', 'opus_decode')     # what runtime/nnaudio.c resolves
# DLLs every Windows 10/11 has: the only imports accepted (lower case; api-ms-win-crt-* are the Universal CRT forwarders)
SYSTEM_DLLS = {'kernel32.dll', 'ntdll.dll', 'user32.dll', 'advapi32.dll', 'ucrtbase.dll', 'msvcrt.dll', 'bcrypt.dll'}

# The movie fallback (runtime/mf_ffmpeg.c): an LGPL FFmpeg 8.1.3 with only the WebM demuxer and the VP9 / H.264 / Vorbis /
# Opus decoders, built by scripts/ffmpeg/build_ffmpeg_win.sh and packed by scripts/ffmpeg/package_ffmpeg.py (bin/ with the
# five DLLs, include/ with the headers the runtime is compiled against, COPYING.LGPLv2.1, README.txt).
FFMPEG_ZIP = 'ssbu-ffmpeg-8.1.3-lgpl-win64.zip'
FFMPEG_URL = MIRROR + FFMPEG_ZIP
FFMPEG_ZIP_SHA256 = '47f0bb9ec3e1d3095625bb3272e7005d203b33e415d475fe051e9fe66ea69171'
FFMPEG_DLLS = {     # load order: each one imports only the ones before it and Windows' own DLLs
    'avutil-60.dll': '5e492b29140b9cb5801ba2c0c4a5c6b2f29984638ee488c0c520991ad83f34c6',
    'swresample-6.dll': 'e400d8dfaee5873677de9766ce7d65b6eb51aa5a0521392e70d13f7019ffa2e7',
    'swscale-9.dll': 'cac9a364f305c31af13bf6de75060ef2d32d4a528cdf32fe586b6ae4e36e0547',
    'avcodec-62.dll': 'f9a2e399f407a1ac33abfbf1b36347c288463cb8ba4b9fdb2da1f02b2ff3cd5c',
    'avformat-62.dll': '96a1d5998096c87b41252ab2274853fe8c9e5fa529eb84bef3f51d3e3ab5cfc7',
}

CODECS = [
    ('Microsoft.VP9VideoExtensions', 'VP9 Video Extensions', 'https://apps.microsoft.com/detail/9n4d0msmp0pt',
     'ms-windows-store://pdp/?productid=9N4D0MSMP0PT'),
    ('Microsoft.WebMediaExtensions', 'Web Media Extensions', 'https://apps.microsoft.com/detail/9n5tdp8vcmhs',
     'ms-windows-store://pdp/?productid=9N5TDP8VCMHS'),
]


def say(msg=''):
    print(msg.encode('ascii', 'replace').decode('ascii'), flush=True)


def sha256_bytes(b):
    return hashlib.sha256(b).hexdigest()


def get(url, timeout):
    """The bytes at url. When Python's certificate check fails (issue #13: a root Windows had not fetched yet), Windows'
    curl.exe gets a second try: it checks with Windows, which fetches the root. Callers check the SHA-256 either way."""
    req = urllib.request.Request(url, headers={'User-Agent': 'ssbu-recomp-fetch-media'})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.read()
    except OSError as e:
        reason = getattr(e, 'reason', e)
        if not (isinstance(reason, ssl.SSLError) or 'CERTIFICATE_VERIFY_FAILED' in str(e)):
            raise
    curl = os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'curl.exe')
    if not os.path.isfile(curl):
        curl = shutil.which('curl') or curl
    r = subprocess.run([curl, '-L', '--fail', '--silent', '--show-error', '--max-time', str(timeout), url], capture_output=True)
    if r.returncode:
        raise OSError('curl exit %d: %s' % (r.returncode, r.stderr.decode('utf-8', 'replace').strip()))
    return r.stdout


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


# --------------------------------------------------------------------------------------------- PE inspection
def pe_info(path):
    """(machine, imported DLL names, exported names) read from the PE headers; no external tool needed."""
    with open(path, 'rb') as f:
        d = f.read()
    if d[:2] != b'MZ':
        raise ValueError('not a PE file')
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    if d[pe:pe + 4] != b'PE\0\0':
        raise ValueError('not a PE file')
    machine, nsec, _, _, _, optsz, _ = struct.unpack_from('<HHIIIHH', d, pe + 4)
    opt = pe + 24
    magic = struct.unpack_from('<H', d, opt)[0]
    dd = opt + (112 if magic == 0x20b else 96)
    exp_rva, exp_size = struct.unpack_from('<II', d, dd)
    imp_rva, _ = struct.unpack_from('<II', d, dd + 8)
    secs = []
    so = opt + optsz
    for i in range(nsec):
        vsz, va, rsz, raw = struct.unpack_from('<IIII', d, so + 40 * i + 8)
        secs.append((va, max(vsz, rsz), raw))

    def off(rva):
        for va, sz, raw in secs:
            if va <= rva < va + sz:
                return rva - va + raw
        raise ValueError('bad RVA')

    def cstr(o):
        e = d.index(b'\0', o)
        return d[o:e].decode('ascii', 'replace')

    imports, exports = [], []
    if imp_rva:
        o = off(imp_rva)
        while True:
            _, _, _, name, _ = struct.unpack_from('<IIIII', d, o)
            if not name:
                break
            imports.append(cstr(off(name)))
            o += 20
    if exp_rva:
        o = off(exp_rva)
        nnames, _, names_rva = struct.unpack_from('<III', d, o + 24)
        no = off(names_rva)
        for i in range(nnames):
            exports.append(cstr(off(struct.unpack_from('<I', d, no + 4 * i)[0])))
    return machine, imports, exports


def check_dll(path):
    """Raise ValueError unless path is an x64 DLL that imports only system DLLs and exports what nnaudio.c needs."""
    machine, imports, exports = pe_info(path)
    if machine != 0x8664:
        raise ValueError('not an x64 DLL')
    bad = [n for n in imports if not (n.lower() in SYSTEM_DLLS or n.lower().startswith('api-ms-win-'))]
    if bad:
        raise ValueError('depends on non-system DLLs: ' + ', '.join(bad))
    miss = [n for n in OPUS_EXPORTS if n not in exports]
    if miss:
        raise ValueError('does not export: ' + ', '.join(miss))
    return imports


# ------------------------------------------------------------------------------------------- unpack .pkg.tar.zst
def _members_zstandard(blob, wanted):
    try:
        import zstandard
    except ImportError:
        say('  installing the Python module zstandard for your user ...')
        r = subprocess.run([sys.executable, '-m', 'pip', 'install', '--user', '--quiet', 'zstandard'], capture_output=True)
        if r.returncode:
            return None
        import importlib
        importlib.invalidate_caches()
        try:
            import zstandard
        except ImportError:
            return None
    out = {}
    with zstandard.ZstdDecompressor().stream_reader(io.BytesIO(blob)) as rd, tarfile.open(fileobj=rd, mode='r|') as t:
        for m in t:
            if m.name in wanted and m.isfile():
                out[m.name] = t.extractfile(m).read()
    return out


def _members_tar_exe(blob, wanted):
    """Windows 10/11 ship bsdtar (tar.exe), which reads .zst."""
    tar = os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'tar.exe')
    if not os.path.isfile(tar):
        return None
    with tempfile.TemporaryDirectory(prefix='ssbu_zst_') as tmp:
        pkg = os.path.join(tmp, 'p.tar.zst')
        with open(pkg, 'wb') as f:
            f.write(blob)
        out_dir = os.path.join(tmp, 'o')
        os.mkdir(out_dir)
        # bsdtar exits 1 when one listed member is absent but still extracts the others: judge by the files, not by the code
        subprocess.run([tar, '-xf', pkg, '-C', out_dir] + sorted(wanted), capture_output=True, cwd=tmp)
        out = {}
        for w in wanted:
            p = os.path.join(out_dir, *w.split('/'))
            if os.path.isfile(p):
                with open(p, 'rb') as f:
                    out[w] = f.read()
        return out


def unpack_members(blob, wanted):
    for fn in (_members_zstandard, _members_tar_exe):
        try:
            got = fn(blob, set(wanted))
        except Exception as e:                      # a broken module / archive: try the next way
            say('  (%s failed: %s)' % (fn.__name__, e))
            got = None
        if got and OPUS_DLL_MEMBER in got:
            return got
    raise RuntimeError('cannot unpack the .zst package: install the Python module zstandard (pip install --user zstandard)')


# ------------------------------------------------------------------------------------------------- fetch_opus
def fetch_opus(deps_dir=None, force=False, url=OPUS_URL, pkg_sha256=OPUS_PKG_SHA256, dll_sha256=OPUS_DLL_SHA256):
    """Put a verified libopus-0.dll in <deps_dir>/opus/ and return its path; None if it could not be done (a message says
    why: the game then runs with silent music and nothing else is affected). An existing, valid copy is kept."""
    deps_dir = deps_dir or os.path.join(REPO_DIR, 'deps')
    dest_dir = os.path.join(deps_dir, 'opus')
    dest = os.path.join(dest_dir, 'libopus-0.dll')
    try:
        if os.path.isfile(dest) and not force and sha256_file(dest) == dll_sha256:
            check_dll(dest)
            say('libopus: already in %s (checked)' % dest)
            return dest
        say('libopus: downloading %s' % url)
        blob = get(url, 120)
        got = sha256_bytes(blob)
        if got != pkg_sha256:
            say('libopus: the downloaded package does not match its pinned SHA-256 (got %s); NOT used.' % got)
            return None
        files = unpack_members(blob, (OPUS_DLL_MEMBER, OPUS_LICENSE_MEMBER))
        dll = files[OPUS_DLL_MEMBER]
        if sha256_bytes(dll) != dll_sha256:
            say('libopus: the DLL inside the package does not match its pinned SHA-256; NOT used.')
            return None
        os.makedirs(dest_dir, exist_ok=True)
        tmp = dest + '.tmp'
        with open(tmp, 'wb') as f:
            f.write(dll)
        try:
            imports = check_dll(tmp)
        except Exception as e:                  # a truncated / odd file: struct and index errors are not ValueError
            os.remove(tmp)
            say('libopus: rejected (%s).' % e)
            return None
        os.replace(tmp, dest)
        if OPUS_LICENSE_MEMBER in files:
            with open(os.path.join(dest_dir, 'LICENSE-opus.txt'), 'wb') as f:
                f.write(files[OPUS_LICENSE_MEMBER])
        with open(os.path.join(dest_dir, 'SOURCE.txt'), 'w') as f:
            f.write('libopus-0.dll from %s\npackage sha256 %s\ndll sha256 %s\nOpus is BSD licensed (LICENSE-opus.txt).\n'
                    % (url, pkg_sha256, dll_sha256))
        say('libopus: ok, %s (imports only system DLLs: %d; exports %s)' % (dest, len(imports), ', '.join(OPUS_EXPORTS)))
        return dest
    except Exception as e:
        say('libopus: could not be fetched (%s). The game runs without music; try again later, or give setup.py '
            '--opus <opus.dll or its folder>.' % e)
        return None


# ----------------------------------------------------------------------------------------------- fetch_ffmpeg
def ffmpeg_ready(deps_dir=None):
    """True when <deps>/ffmpeg/bin holds the five pinned DLLs and include/ the headers (nothing is downloaded)."""
    d = os.path.join(deps_dir or os.path.join(REPO_DIR, 'deps'), 'ffmpeg')
    try:
        return (os.path.isfile(os.path.join(d, 'include', 'libavcodec', 'avcodec.h')) and
                all(sha256_file(os.path.join(d, 'bin', n)) == h for n, h in FFMPEG_DLLS.items()))
    except OSError:
        return False


def fetch_ffmpeg(deps_dir=None, force=False, url=FFMPEG_URL, zip_sha256=FFMPEG_ZIP_SHA256):
    """Put the movie fallback's FFmpeg (bin/ DLLs, include/, licence) in <deps_dir>/ffmpeg/ and return that folder; None
    if it could not be done (a message says why: the movies then play only where Windows' own codecs can, as before)."""
    deps_dir = deps_dir or os.path.join(REPO_DIR, 'deps')
    dest = os.path.join(deps_dir, 'ffmpeg')
    try:
        if not force and ffmpeg_ready(deps_dir):
            say('FFmpeg (movie fallback): already in %s (checked)' % dest)
            return dest
        say('FFmpeg (movie fallback): downloading %s' % url)
        blob = get(url, 180)
        got = sha256_bytes(blob)
        if got != zip_sha256:
            say('FFmpeg: the downloaded package does not match its pinned SHA-256 (got %s); NOT used.' % got)
            return None
        import zipfile
        tmp = dest + '.tmp'
        shutil.rmtree(tmp, ignore_errors=True)
        with zipfile.ZipFile(io.BytesIO(blob)) as z:
            for m in z.infolist():
                name = m.filename
                if m.is_dir():
                    continue
                parts = name.split('/')
                if name.startswith('/') or '..' in parts or ':' in name or not (parts[0] in ('bin', 'include') or len(parts) == 1):
                    say('FFmpeg: unexpected file in the package (%s); NOT used.' % name)
                    return None
                out = os.path.join(tmp, *parts)
                os.makedirs(os.path.dirname(out), exist_ok=True)
                with open(out, 'wb') as f:
                    f.write(z.read(m))
        own = set(n.lower() for n in FFMPEG_DLLS)
        for n, h in FFMPEG_DLLS.items():
            p = os.path.join(tmp, 'bin', n)
            if not os.path.isfile(p) or sha256_file(p) != h:
                shutil.rmtree(tmp, ignore_errors=True)
                say('FFmpeg: %s is missing from the package or does not match its pinned SHA-256; NOT used.' % n)
                return None
            machine, imports, _ = pe_info(p)
            bad = [i for i in imports if not (i.lower() in SYSTEM_DLLS or i.lower() in own or i.lower().startswith('api-ms-win-'))]
            if machine != 0x8664 or bad:
                shutil.rmtree(tmp, ignore_errors=True)
                say('FFmpeg: %s rejected (%s); NOT used.' % (n, 'not x64' if machine != 0x8664 else 'imports ' + ', '.join(bad)))
                return None
        with open(os.path.join(tmp, 'SOURCE.txt'), 'w') as f:
            f.write('FFmpeg 8.1.3 (LGPL 2.1+, COPYING.LGPLv2.1) from %s\npackage sha256 %s\nHow it was built: README.txt, '
                    'scripts/ffmpeg/build_ffmpeg_win.sh\n' % (url, zip_sha256))
        shutil.rmtree(dest, ignore_errors=True)
        os.replace(tmp, dest)
        say('FFmpeg (movie fallback): ok, %s (5 DLLs checked; they import only Windows\' own DLLs and each other)' % dest)
        return dest
    except Exception as e:
        say('FFmpeg (movie fallback): could not be fetched (%s). The movies play where Windows\' own codecs can; '
            'run the installer again later to add it.' % e)
        return None


# ----------------------------------------------------------------------------------------------- check_codecs
def check_codecs(verbose=True):
    """{package name: True/False/None} for the Store codec extensions (None: could not ask). Prints what to do if missing."""
    res = {}
    if os.name != 'nt':
        return res
    for pkg, title, web, store in CODECS:
        try:
            r = subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command',
                                '(Get-AppxPackage -Name %s | Measure-Object).Count' % pkg],
                               capture_output=True, text=True, timeout=60)
            res[pkg] = int((r.stdout or '0').strip() or 0) > 0 if r.returncode == 0 else None
        except (OSError, ValueError, subprocess.TimeoutExpired):
            res[pkg] = None
    if verbose:
        for pkg, title, web, store in CODECS:
            st = res.get(pkg)
            say('codec  %-22s %s' % (title, {True: 'installed', False: 'MISSING', None: 'could not be checked'}[st]))
            if st is not True:
                say('        get it (free) from the Microsoft Store: %s   (or open %s)' % (web, store))
        if not all(v is True for v in res.values()):
            say('Without them the game plays its movies with FFmpeg (downloaded with the rest; nothing to do). To use Windows\'\n'
                'own decoders instead, get them from the Microsoft Store (Get / Install there; once per PC, no restart).')
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--deps', help='deps folder (default <repo>/deps); libopus goes to <deps>/opus/')
    ap.add_argument('--force', action='store_true', help='download again even if a valid libopus-0.dll is there')
    ap.add_argument('--check-codecs', action='store_true', help='only report the Store codecs, download nothing')
    ap.add_argument('--no-codecs', action='store_true', help='do not check the Store codecs')
    ap.add_argument('--no-ffmpeg', action='store_true', help='do not fetch the FFmpeg movie fallback')
    a = ap.parse_args()
    ok = True
    if not a.check_codecs:
        ok = fetch_opus(a.deps, a.force) is not None
        if not a.no_ffmpeg and os.name == 'nt':
            ok = fetch_ffmpeg(a.deps, a.force) is not None and ok
    if not a.no_codecs:
        check_codecs()
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
