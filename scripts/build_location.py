#!/usr/bin/env python3
"""Where the big files go: the build folder (the unpacked game ~17 GB, the build ~8 GB, the logs, the saves, the
temporary .nsz decompression). Default <repo>\\build. The installer window lets you choose another folder (another drive
with more space); the choice is kept in build_location.txt at the repository root (one line: the folder), so that
scripts/install.py, setup.py, run.py, precheck.py, the launcher, the two exes at the top of the folder (launcher/start.c)
and the shortcuts all use the same
place, and Resume goes on in it.

    python scripts/build_location.py            print the build folder in use
    python scripts/build_location.py --set DIR  remember DIR (the default folder removes build_location.txt)

Small files that are written before a location can be chosen stay in <repo>\\build whatever the choice:
python_path.txt (scripts/bootstrap.ps1), downloads\\ and update_manifest.json (scripts/update.py).
Only the standard library is used.
"""
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FILE_NAME = 'build_location.txt'


def default(repo=REPO):
    return os.path.join(repo, 'build')


def same(a, b):
    # realpath: one folder written two ways (a short 8.3 name and the long name, or a junction) is the same folder
    return os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))


def stored(repo=REPO):
    """The folder written in build_location.txt, or None (no file, empty, unreadable)."""
    try:
        with open(os.path.join(repo, FILE_NAME), encoding='utf-8-sig') as f:
            line = f.readline().strip().strip('"')
    except (OSError, UnicodeDecodeError):
        return None
    return os.path.abspath(os.path.expandvars(line)) if line else None


def get(repo=REPO):
    """The build folder in use: build_location.txt, else <repo>\\build."""
    return stored(repo) or default(repo)


def save(path, repo=REPO):
    """Remember `path` (absolute). The default folder removes the file, so nothing changes for whoever never chose one."""
    f = os.path.join(repo, FILE_NAME)
    path = os.path.abspath(path)
    if same(path, default(repo)):
        if os.path.exists(f):
            os.remove(f)
        return path
    tmp = f + '.tmp'
    with open(tmp, 'w', encoding='utf-8', newline='\r\n') as fh:
        fh.write(path + '\n')
    os.replace(tmp, f)
    return path


def main(argv):
    if argv[:1] == ['--set'] and len(argv) == 2:
        print(save(argv[1]))
        return 0
    if argv:
        print(__doc__)
        return 2
    print(get())
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
