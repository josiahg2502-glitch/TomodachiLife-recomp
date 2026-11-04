#!/usr/bin/env python3
"""Checks before the installation starts, with plain messages for people who are not programmers.

    python scripts/precheck.py [--build DIR] [--need-msvc] [--json]

Prints one line per check ("OK   Windows: ...", "WARN ...", "FAIL ...") and exits 1 if any check fails.
As a module: checks(build_dir, need_msvc=False, dump_dir=None) -> list of dict(level='ok'|'warn'|'fail', what=str, msg=str).
Only the standard library is used, so it runs on a fresh Python 3.10+ (also the python.org embeddable package).
"""
import argparse
import ctypes
import json
import os
import platform
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# CPU: what the code really needs. The game libraries are built by clang 9 with the default x86-64 target and no -m flags
# (tools/lifter/build.py, build_opt.py) and the host runtime by MSVC with its default /arch (SSE2); nothing in runtime/ uses
# intrinsics (no immintrin / _mm_ / __builtin_ia32 / cpuid). So the real requirement is SSE2, which every 64-bit x86 CPU has.
# AVX2 / SSE4.2 are only reported: Windows 11 itself wants SSE4.2 + POPCNT, and the graphics driver may use AVX.
PF_SSE2, PF_SSE3, PF_SSE4_2, PF_AVX, PF_AVX2 = 10, 13, 38, 39, 40

# Disk, measured 2026-10-19 on two complete builds (GB = 2**30 bytes):
#   work_rel/build_user:       5.22 GB (work 5.05: gen_eh_o2 1.30, gen_nro_eh_o2 1.82, obj_eh_o1 0.60, obj_nro_eh_o2 0.68,
#                              game dll 0.50, rest 0.2; binaries 0.14, sysdata 0.02)
#   work_1305/pr11/build:      7.88 GB (same plus cache 1.06 and a bigger work folder 6.68)
# The larger one, rounded up, is what a build needs; the dump made from .nsp packages (build/dump) is about 17 GB.
BUILD_NEED_GB = 4.0
DUMP_NEED_GB = 7.0
# RAM: clang -O1 on the largest lifted files takes about 1.5 GB per parallel job (rule of thumb, not measured per file).
GB_PER_JOB = 1.75
RAM_WARN_GB = 16
RAM_FAIL_GB = 8

STORE_VP9 = 'ms-windows-store://pdp/?ProductId=9N4D0MSMP0PT'
STORE_WEB = 'ms-windows-store://pdp/?ProductId=9N5TDP8VCMHS'


def _r(level, what, msg):
    return {'level': level, 'what': what, 'msg': msg}


def _win():
    return os.name == 'nt'


def check_windows():
    if not _win():
        return _r('fail', 'Windows', 'this installer only works on Windows 10 or 11 (64-bit)')
    arch = os.environ.get('PROCESSOR_ARCHITEW6432') or os.environ.get('PROCESSOR_ARCHITECTURE') or platform.machine()
    if arch.upper() not in ('AMD64', 'X86_64'):
        return _r('fail', 'Windows', 'a 64-bit Windows on an Intel/AMD processor is needed (found %s)' % arch)
    v = sys.getwindowsversion()
    if v.major < 10:
        return _r('fail', 'Windows', 'Windows 10 or 11 is needed (found Windows %d.%d); please update Windows' % (v.major, v.minor))
    name = 'Windows 11' if v.build >= 22000 else 'Windows 10'
    return _r('ok', 'Windows', '%s 64-bit (build %d)' % (name, v.build))


def _pf(n):
    try:
        return bool(ctypes.windll.kernel32.IsProcessorFeaturePresent(n))
    except Exception:
        return None


def check_cpu():
    if not _win():
        return _r('warn', 'Processor', 'could not be checked on this system')
    if _pf(PF_SSE2) is False:
        return _r('fail', 'Processor', 'the processor is too old (SSE2 missing)')
    feats = [n for n, p in (('SSE3', PF_SSE3), ('SSE4.2', PF_SSE4_2), ('AVX', PF_AVX), ('AVX2', PF_AVX2)) if _pf(p)]
    name = os.environ.get('PROCESSOR_IDENTIFIER', 'x86-64 processor')
    if _pf(PF_AVX2):
        return _r('ok', 'Processor', '%s (%s)' % (name, ', '.join(feats)))
    # AVX2 is not required by our code: say so, but it may mean a slow old machine
    return _r('warn', 'Processor', 'works, but it is an old model (no AVX2); the game may run slowly. Found: %s' % (', '.join(feats) or 'basic features only'))


def _ram_gb():
    class MS(ctypes.Structure):
        _fields_ = [('dwLength', ctypes.c_ulong), ('dwMemoryLoad', ctypes.c_ulong), ('ullTotalPhys', ctypes.c_ulonglong),
                    ('ullAvailPhys', ctypes.c_ulonglong), ('ullTotalPageFile', ctypes.c_ulonglong),
                    ('ullAvailPageFile', ctypes.c_ulonglong), ('ullTotalVirtual', ctypes.c_ulonglong),
                    ('ullAvailVirtual', ctypes.c_ulonglong), ('ullAvailExtendedVirtual', ctypes.c_ulonglong)]
    m = MS()
    m.dwLength = ctypes.sizeof(MS)
    if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m)):
        return None
    return m.ullTotalPhys / 2.0 ** 30


def suggested_jobs(ram_gb):
    cpus = os.cpu_count() or 2
    by_ram = int((ram_gb or 8) // GB_PER_JOB)
    return max(1, min(cpus, by_ram))


def check_ram():
    try:
        gb = _ram_gb()
    except Exception:
        gb = None
    if gb is None:
        return _r('warn', 'Memory', 'could not be read; if the build stops with "out of memory", use --jobs 2')
    jobs = suggested_jobs(gb)
    # a compiler job = one compiler program running; each takes a CPU thread and up to about GB_PER_JOB of RAM, so the
    # safe maximum is the smaller of the two counts (more can run the PC out of memory and make compiles fail)
    hint = 'safe: 1 to %d compiler jobs; suggested: --jobs %d' % (jobs, jobs)
    if gb < RAM_FAIL_GB - 0.85:     # an "8 GB" PC reports 7.2-7.9 GB (memory kept by the firmware / integrated GPU): issue #55
        return _r('fail', 'Memory', '%.1f GB of RAM is too little; at least %d GB are needed (16 GB or more is comfortable)' % (gb, RAM_FAIL_GB))
    if gb < RAM_WARN_GB:
        return _r('warn', 'Memory', '%.1f GB of RAM: enough, but the build will be slow and tight; close other programs (%s)' % (gb, hint))
    return _r('ok', 'Memory', '%.1f GB of RAM (%s)' % (gb, hint))


def _existing_parent(p):
    p = os.path.abspath(p)
    while p and not os.path.exists(p):
        up = os.path.dirname(p)
        if up == p:
            break
        p = up
    return p


def nsz_temp_gb(paths):
    """Space a .nsz decompression needs for a while (romfs_extract.py writes a plain .nsp about as big as the game file,
    a bit more since the .nsz is compressed; scripts/install.py removes it after the dump is made). 0 without .nsz."""
    tot = 0
    for p in paths or ():
        try:
            if p and p.lower().endswith('.nsz') and os.path.isfile(p):
                tot += os.path.getsize(p)
        except OSError:
            pass
    return round(tot * 1.25 / 2.0 ** 30, 1)


def disk_need(dump_dir=None, temp_gb=0.0):
    """-> (GB needed in the install location, the parts as text)."""
    need, parts = BUILD_NEED_GB, ['build %d GB' % BUILD_NEED_GB]
    if not (dump_dir and os.path.isdir(dump_dir)):
        need += DUMP_NEED_GB
        parts.append('game files from the .nsp packages %d GB' % DUMP_NEED_GB)
        if temp_gb:
            need += temp_gb
            parts.append('%.0f GB for a while to decompress the .nsz' % temp_gb)
    return need, parts


def check_disk(build_dir, dump_dir=None, temp_gb=0.0):
    """The free space of the drive of the install location (build_dir: <repo>\\build or the folder chosen in the window)."""
    probe = _existing_parent(build_dir)
    need, parts = disk_need(dump_dir, temp_gb)
    try:
        free = shutil.disk_usage(probe).free / 2.0 ** 30
    except OSError:
        return dict(_r('warn', 'Disk space', 'could not be read for %s' % probe), need_gb=need, build_gb=BUILD_NEED_GB, dump_gb=DUMP_NEED_GB)
    drive = os.path.splitdrive(probe)[0] or probe
    txt = '%.0f GB free on %s, %.0f GB needed (%s)' % (free, drive, need, ' + '.join(parts))
    # need_gb / build_gb / dump_gb / free_gb: the installer window shows them next to its "Install location" field
    extra = {'need_gb': need, 'build_gb': BUILD_NEED_GB, 'dump_gb': DUMP_NEED_GB, 'free_gb': round(free, 1), 'drive': drive}
    if free < need:
        return dict(_r('fail', 'Disk space', txt + '; free some space or choose another install location (another drive)'), **extra)
    if free < need * 1.25:
        return dict(_r('warn', 'Disk space', txt + '; it fits, but only just'), **extra)
    return dict(_r('ok', 'Disk space', txt), **extra)


def check_vulkan():
    root = os.environ.get('SystemRoot', r'C:\Windows')
    if os.path.exists(os.path.join(root, 'System32', 'vulkan-1.dll')):
        return _r('ok', 'Graphics (Vulkan)', 'vulkan-1.dll found')
    return _r('warn', 'Graphics (Vulkan)', 'Vulkan was not found: update the graphics card driver (NVIDIA, AMD or Intel) from its maker\'s site')


def check_codecs():
    # Get-AppxPackage takes one name only, so list the names and filter here
    ps = 'Get-AppxPackage | ForEach-Object { $_.Name }'
    try:
        r = subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command', ps], capture_output=True, text=True, errors='replace', timeout=30)
        names = (r.stdout or '').lower() if not r.returncode else ''
    except (OSError, subprocess.TimeoutExpired):
        names = ''
    if not names.strip():                    # PowerShell failed or printed nothing: say so, do not claim the codecs are missing
        return _r('warn', 'Video codecs', 'could not be checked; if the game movies stay black, install "VP9 Video Extensions" and "Web Media Extensions" '
                  'from the Microsoft Store (%s , %s)' % (STORE_VP9, STORE_WEB))
    miss = []
    if 'vp9videoextensions' not in names:
        miss.append('VP9 Video Extensions: ' + STORE_VP9)
    if 'webmediaextensions' not in names:
        miss.append('Web Media Extensions: ' + STORE_WEB)
    if miss:
        return _r('warn', 'Video codecs', 'the game movies need free Microsoft Store add-ons; install: ' + ' | '.join(miss))
    return _r('ok', 'Video codecs', 'VP9 Video Extensions and Web Media Extensions are installed')


def check_msvc(need):
    if not need:
        return _r('ok', 'Visual Studio tools', 'not needed (prebuilt runtime)')
    vsw = os.path.join(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'), 'Microsoft Visual Studio', 'Installer', 'vswhere.exe')
    found = ''
    if os.path.exists(vsw):
        try:
            found = subprocess.run([vsw, '-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                                    '-property', 'installationPath'], capture_output=True, text=True, errors='replace', timeout=60).stdout.strip()
        except (OSError, subprocess.TimeoutExpired):
            pass
    if found:
        return _r('ok', 'Visual Studio tools', found)
    return _r('fail', 'Visual Studio tools', 'the Microsoft C++ Build Tools are missing (needed only without the prebuilt runtime): '
              'install them (about 3 GB on C:, 5-15 minutes, a progress window shows): winget install --id '
              'Microsoft.VisualStudio.2022.BuildTools -e --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools '
              '--includeRecommended", then run this again')


def _long_paths():
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'SYSTEM\CurrentControlSet\Control\FileSystem') as k:
            return bool(winreg.QueryValueEx(k, 'LongPathsEnabled')[0])
    except Exception:
        return None


def check_paths(build_dir):
    bad = []
    for p in (REPO, os.path.abspath(build_dir)):
        try:
            p.encode('ascii')
        except UnicodeEncodeError:
            bad.append(p)
    if bad:
        return _r('warn', 'Folder names', 'this path has accented or special letters: %s ; some tools may fail on it. Use a plain folder such as C:\\ssbu' % bad[0])
    if _win() and len(os.path.abspath(build_dir)) > 80 and _long_paths() is False:
        return _r('warn', 'Folder names', 'the build folder path is long and Windows long paths are off; use a shorter folder such as C:\\ssbu')
    return _r('ok', 'Folder names', 'plain letters only')


def checks(build_dir, need_msvc=False, dump_dir=None, temp_gb=0.0):
    out = []
    for fn in (check_windows, check_cpu, check_ram, lambda: check_disk(build_dir, dump_dir, temp_gb), check_vulkan, check_codecs,
               lambda: check_msvc(need_msvc), lambda: check_paths(build_dir)):
        try:
            out.append(fn())
        except Exception as e:  # a broken check must not stop the others
            out.append(_r('warn', getattr(fn, '__name__', 'check'), 'could not be checked (%s)' % e))
    return out


def main():
    ap = argparse.ArgumentParser(description='Pre-installation checks')
    ap.add_argument('--build', default=None, help='the install location, where the build will be made (default: the one in '
                                                  'build_location.txt, else <repo>\\build); its drive is the one checked')
    ap.add_argument('--dump', default=None, help='folder of an existing dump (then its size is not counted)')
    ap.add_argument('--game', action='append', default=[], help='a game file to be unpacked (repeatable): a .nsz adds the space '
                                                                'its decompression takes for a while')
    ap.add_argument('--need-msvc', action='store_true', help='also check the Microsoft C++ Build Tools')
    ap.add_argument('--json', action='store_true', help='print the result as JSON')
    a = ap.parse_args()
    if not a.build:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import build_location
        a.build = build_location.get()
    res = checks(a.build, a.need_msvc, a.dump, 0.0 if a.dump else nsz_temp_gb(a.game))
    if a.json:
        print(json.dumps(res, indent=1))
    else:
        for c in res:
            print(('%-4s %s: %s' % (c['level'].upper(), c['what'], c['msg'])).encode('ascii', 'replace').decode('ascii'))
    return 1 if any(c['level'] == 'fail' for c in res) else 0


if __name__ == '__main__':
    sys.exit(main())
