#!/usr/bin/env python3
"""Build "dist/Tomodachi-Life-PC-Setup.exe", the one download for players (and dist/Tomodachi-Life-PC.zip, the same files as a
zip): launcher/setup.c compiled with clang, and in its
resources the release's files (FILES: `git archive` of <ref> plus build/work/TomodachiLauncher.exe and SDL3.dll from the
runtime package that <ref>'s scripts/install.py pins, and at the top "Tomodachi Life.exe" / "Tomodachi Life Launcher.exe",
launcher/start.c compiled here), a manifest (common controls 6 for its dialog, no elevation, DPI
aware), an icon (a smiling face drawn here: no game art is shipped) and version information.

    python scripts/make_setup.py [--ref v1.0.0] [--clang <clang.exe>] [--out dist]

The runtime package must be in --out (make_release.py writes it there) with the SHA-256 that <ref> pins.
"""
import argparse
import ctypes
import hashlib
import io
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
from ctypes import wintypes

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PREFIX = 'Tomodachi-Life-PC/'
NAME = 'Tomodachi-Life-PC-Setup.exe'
TOP_EXES = [('Tomodachi Life.exe', ['-DSTART_PLAY']), ('Tomodachi Life Launcher.exe', [])]      # in place of 1.0.0's .bat files
MANIFEST = b'''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity type="win32" name="TomodachiLifePC.Setup" version="1.0.0.0" processorArchitecture="amd64"/>
  <description>Tomodachi Life PC Setup</description>
  <dependency><dependentAssembly><assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls" version="6.0.0.0"
    processorArchitecture="*" publicKeyToken="6595b64144ccf1df" language="*"/></dependentAssembly></dependency>
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3"><security><requestedPrivileges>
    <requestedExecutionLevel level="asInvoker" uiAccess="false"/></requestedPrivileges></security></trustInfo>
  <application xmlns="urn:schemas-microsoft-com:asm.v3"><windowsSettings>
    <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true/pm</dpiAware>
    <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2</dpiAwareness></windowsSettings></application>
  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1"><application>
    <supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/></application></compatibility>
</assembly>
'''


def git(*args):
    return subprocess.run(['git', '-C', REPO] + list(args), check=True, capture_output=True).stdout


def payload(ref, out, top):
    """the zip the setup unpacks: the repository at ref, the launcher and SDL3.dll of the runtime package ref pins, and
    `top` ({name: bytes}) at the top of the folder"""
    install = git('show', '%s:scripts/install.py' % ref).decode('utf-8')
    m = re.search(r"^RUNTIME_RELEASE = \(MIRROR \+ '([^']+)', '([0-9a-f]{64})'\)", install, re.M)
    if not m:
        sys.exit('%s: no pinned runtime package in scripts/install.py' % ref)
    pkg = os.path.join(out, m.group(1))
    if not os.path.isfile(pkg):
        sys.exit('%s pins %s: not in %s (scripts/make_release.py writes it)' % (ref, m.group(1), out))
    if hashlib.sha256(open(pkg, 'rb').read()).hexdigest() != m.group(2):
        sys.exit('%s does not have the SHA-256 that %s pins' % (pkg, ref))
    buf = io.BytesIO(git('archive', '--format=zip', '--prefix=' + PREFIX, ref))
    with zipfile.ZipFile(pkg) as rt, zipfile.ZipFile(buf, 'a', zipfile.ZIP_DEFLATED) as z:
        for name in ('TomodachiLauncher.exe', 'SDL3.dll'):
            z.writestr(PREFIX + 'build/work/' + name, rt.read(name))
        for name, data in top.items():
            z.writestr(PREFIX + name, data)
    return buf.getvalue(), m.group(1)


def icon():
    """a yellow smiling face, 16-256 px, as .ico bytes"""
    from PIL import Image, ImageDraw
    big = Image.new('RGBA', (256, 256), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    d.ellipse((8, 8, 248, 248), fill=(255, 200, 40, 255), outline=(240, 120, 20, 255), width=12)
    d.ellipse((76, 70, 108, 118), fill=(60, 40, 20, 255))
    d.ellipse((148, 70, 180, 118), fill=(60, 40, 20, 255))
    d.arc((60, 80, 196, 200), 25, 155, fill=(60, 40, 20, 255), width=14)
    f = io.BytesIO()
    big.save(f, format='ICO', sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    return f.getvalue()


def version_info(ver, desc):
    """VS_VERSIONINFO as resource bytes: Windows' file properties show what this program is"""
    def block(key, value=b'', vtype=1, vlen=None, children=b''):
        k = key.encode('utf-16-le') + b'\0\0'
        head = k + b'\0' * (-(6 + len(k)) % 4)
        body = value + b'\0' * (-len(value) % 4)
        return struct.pack('<HHH', 6 + len(head) + len(body) + len(children), len(value) if vlen is None else vlen, vtype) + head + body + children
    nums = (([int(x) for x in re.findall(r'\d+', ver)] + [0, 0, 0, 0])[:4])
    ms, ls = nums[0] << 16 | nums[1], nums[2] << 16 | nums[3]
    fixed = struct.pack('<13I', 0xFEEF04BD, 0x00010000, ms, ls, ms, ls, 0x3F, 0, 0x00040004, 1, 0, 0, 0)
    strings = b''
    for key, val in (('CompanyName', 'TomodachiLife-recomp project'), ('FileDescription', desc), ('FileVersion', ver), ('LegalCopyright', 'Open source, see LICENSE'),
                     ('ProductName', 'Tomodachi Life PC'), ('ProductVersion', ver)):
        v = val.encode('utf-16-le') + b'\0\0'
        s = block(key, v, 1, len(val) + 1)
        strings += s + b'\0' * (-len(s) % 4)
    sfi = block('StringFileInfo', vtype=1, children=block('040904b0', vtype=1, children=strings))
    vfi = block('VarFileInfo', vtype=1, children=block('Translation', struct.pack('<HH', 0x0409, 1200), 0))
    return block('VS_VERSION_INFO', fixed, 0, children=sfi + b'\0' * (-len(sfi) % 4) + vfi)


def stamp(exe, zipdata, ico, manifest=MANIFEST, version=None):
    """the icon, version information, and the manifest and the FILES when given, into exe's resources"""
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    k32.BeginUpdateResourceW.restype = wintypes.HANDLE
    k32.BeginUpdateResourceW.argtypes = [wintypes.LPCWSTR, wintypes.BOOL]
    k32.UpdateResourceW.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, wintypes.WORD, ctypes.c_char_p, wintypes.DWORD]
    k32.EndUpdateResourceW.argtypes = [wintypes.HANDLE, wintypes.BOOL]
    n = struct.unpack_from('<H', ico, 4)[0]
    entries = [struct.unpack_from('<BBBBHHII', ico, 6 + 16 * i) for i in range(n)]
    group = struct.pack('<HHH', 0, 1, n) + b''.join(struct.pack('<BBBBHHIH', *e[:7], i + 1) for i, e in enumerate(entries))
    h = k32.BeginUpdateResourceW(exe, False)
    ok = bool(h)
    if manifest:
        ok = ok and bool(k32.UpdateResourceW(h, 24, 1, 0, manifest, len(manifest)))                       # RT_MANIFEST
    for i, e in enumerate(entries):
        ok = ok and bool(k32.UpdateResourceW(h, 3, i + 1, 0, ico[e[7]:e[7] + e[6]], e[6]))               # RT_ICON
    ok = ok and bool(k32.UpdateResourceW(h, 14, 1, 0, group, len(group)))                                # RT_GROUP_ICON
    if version:
        ok = ok and bool(k32.UpdateResourceW(h, 16, 1, 0x0409, version, len(version)))                    # RT_VERSION
    if zipdata is not None:
        name = ctypes.create_unicode_buffer('FILES')
        ok = ok and bool(k32.UpdateResourceW(h, 10, ctypes.addressof(name), 0, zipdata, len(zipdata)))     # RT_RCDATA
    ok = bool(h) and bool(k32.EndUpdateResourceW(h, not ok)) and ok
    if not ok:
        sys.exit('the resources could not be written into %s (error %d)' % (exe, ctypes.get_last_error()))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ref', default='HEAD', help='the git tag or commit whose files the setup installs')
    ap.add_argument('--clang', default=os.environ.get('LAUNCHER_CLANG', r'C:\Program Files\LLVM\bin\clang.exe'))
    ap.add_argument('--out', default=os.path.join(REPO, 'dist'))
    a = ap.parse_args()
    ico = icon()
    exe = os.path.join(a.out, NAME)
    ver = git('show', a.ref + ':version.txt').decode().strip()
    with tempfile.TemporaryDirectory() as tmp:
        top = {}
        for name, defs in TOP_EXES:                       # the game and the launcher at the top of the folder (launcher/start.c)
            built = os.path.join(tmp, 'start.exe')
            subprocess.run([a.clang, '-O2', '-w'] + defs + [os.path.join(REPO, 'launcher', 'start.c'), '-o', built, '-luser32',
                            '-Wl,/SUBSYSTEM:WINDOWS', '-Wl,/ENTRY:wWinMainCRTStartup'], check=True)
            stamp(built, None, ico, manifest=None, version=version_info(ver, name[:-4]))
            with open(built, 'rb') as f:
                top[name] = f.read()
        zipdata, pkg = payload(a.ref, a.out, top)
        with open(os.path.join(a.out, 'Tomodachi-Life-PC.zip'), 'wb') as f:      # the same files as a zip (the release's other download)
            f.write(zipdata)
        built = os.path.join(tmp, 'setup.exe')
        subprocess.run([a.clang, '-O2', '-w', '-I' + os.path.join(REPO, 'third_party', 'stb'), os.path.join(REPO, 'launcher', 'setup.c'), '-o', built, '-luser32', '-lshell32', '-lole32',
                        '-lcomctl32', '-luuid', '-Wl,/SUBSYSTEM:WINDOWS', '-Wl,/ENTRY:wWinMainCRTStartup'], check=True)
        stamp(built, zipdata, ico, version=version_info(ver, 'Tomodachi Life PC Setup'))
        shutil.copyfile(built, exe)                       # (the temporary folder may be on another drive)
    commit = git('rev-parse', '--short=10', a.ref + '^{commit}').decode().strip()
    print('%s  (%.1f MB): %s at %s, launcher from %s' % (exe, os.path.getsize(exe) / 1048576, a.ref, commit, pkg))
    print('sha256: %s' % hashlib.sha256(open(exe, 'rb').read()).hexdigest())


if __name__ == '__main__':
    main()
