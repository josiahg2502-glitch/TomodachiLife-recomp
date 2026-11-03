#!/usr/bin/env python3
"""Desktop and Start menu shortcuts for the game and its installer (used by scripts/install.py).

    python scripts/shortcuts.py [--dry-run]

Creates "Tomodachi Life.lnk" (target: the launcher, build/work/TomodachiLauncher.exe of the install location) on the desktop
and in the Start menu (Programs), and "Tomodachi Life - Installer.lnk" (the launcher too: its INSTALL) in the Start menu. Idempotent: a shortcut that
already points at the right place is left alone; one that points elsewhere (the folder was moved) is rewritten.

The python.org embeddable package has no pywin32, so the shortcuts are made through `powershell -NoProfile` with the
WScript.Shell COM object (it ships with Windows). Environment override for tests: SSBU_SHORTCUT_DIRS=<dir>[;<dir>...]
puts all three shortcuts into those folders instead of the real desktop and Start menu.
Only the standard library is used. Nothing is written outside the folders named above.
"""
import json
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLAY_NAME = 'Tomodachi Life.lnk'
INSTALL_NAME = 'Tomodachi Life - Installer.lnk'

_PS = r"""
$ErrorActionPreference = 'Stop'
$sh = New-Object -ComObject WScript.Shell
foreach ($it in (ConvertFrom-Json $env:SSBU_LNK_JSON)) {
  $dir = $it.dir
  if ($dir -eq '@desktop') { $dir = [Environment]::GetFolderPath('Desktop') }
  elseif ($dir -eq '@programs') { $dir = [Environment]::GetFolderPath('Programs') }
  if (-not $dir) { Write-Output ('fail ' + $it.name + ' (no folder)'); continue }
  if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
  $path = Join-Path $dir $it.name
  if (Test-Path -LiteralPath $path) {
    $old = $sh.CreateShortcut($path)
    if ($old.TargetPath -ieq $it.target -and $old.WorkingDirectory -ieq $it.cwd) { Write-Output ('ok ' + $path); continue }
  }
  $l = $sh.CreateShortcut($path)
  $l.TargetPath = $it.target
  $l.WorkingDirectory = $it.cwd
  $l.Description = $it.desc
  if ($it.icon) { $l.IconLocation = $it.icon }
  $l.Save()
  Write-Output ('created ' + $path)
}
"""


def plan(repo=REPO):
    """-> list of dict(dir, name, target, cwd, desc, icon); dir is a real folder or '@desktop' / '@programs'."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import build_location
    # the launcher itself (no console window), of the install location, else the one the release unpacked; it installs what
    # is missing too, so both shortcuts open it (1.0.0's Installer shortcut named Install Tomodachi Life.bat: rewritten)
    want = os.path.join(build_location.get(repo), 'work', 'TomodachiLauncher.exe')
    exe = next((p for p in (want, os.path.join(repo, 'build', 'work', 'TomodachiLauncher.exe')) if os.path.isfile(p)), want)
    # the logo's .ico, since the launcher's own icon cannot be written while it runs the install
    ico = os.path.join(os.path.dirname(exe), 'tomodachi.ico')
    icon = ico if os.path.isfile(ico) else exe + ',0' if os.path.isfile(exe) else ''
    play = {'name': PLAY_NAME, 'target': exe, 'cwd': os.path.dirname(exe), 'desc': 'Tomodachi Life: Living the Dream, native PC build', 'icon': icon}
    inst = {'name': INSTALL_NAME, 'target': exe, 'cwd': os.path.dirname(exe), 'desc': 'Install, repair or update Tomodachi Life native (INSTALL)', 'icon': icon}
    over = [d for d in (os.environ.get('SSBU_SHORTCUT_DIRS') or '').split(os.pathsep) if d]
    if over:
        return [dict(it, dir=d) for d in over for it in (play, inst)]
    return [dict(play, dir='@desktop'), dict(play, dir='@programs'), dict(inst, dir='@programs')]


def create_all(repo=REPO, dry=False):
    """Make the shortcuts; -> list of result lines ('created <path>', 'ok <path>', 'would create <dir>\\<name>', 'fail ...')."""
    items = plan(repo)
    if dry:
        return ['would create %s -> %s' % (os.path.join(it['dir'], it['name']), it['target']) for it in items]
    if os.name != 'nt':
        return ['fail shortcuts are only made on Windows']
    env = dict(os.environ, SSBU_LNK_JSON=json.dumps(items))
    r = subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command', _PS],      # (the execution policy is about script files, not -Command)
                       capture_output=True, text=True, errors='replace', env=env, timeout=120)
    out = [l.strip() for l in r.stdout.splitlines() if l.strip()]
    if r.returncode:
        out.append('fail powershell: ' + (r.stderr.strip().splitlines() or ['exit %d' % r.returncode])[-1])
    return out


def main(argv):
    res = create_all(dry='--dry-run' in argv)
    for l in res:
        print(l.encode('ascii', 'replace').decode('ascii'))
    return 1 if any(l.startswith('fail') for l in res) else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
