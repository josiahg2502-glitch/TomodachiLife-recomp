#!/usr/bin/env python3
"""Bring this folder to the newest commit of the GitHub repository's main branch.

    python scripts/update.py [--dry-run] [--repo owner/name] [--branch main] [--force]

* A git checkout: `git fetch` (from the remote that points at the repository, else from its URL) and a fast-forward
  merge only. Nothing is merged if your own uncommitted changes touch a file the update changes, or if your branch has
  commits GitHub's lacks (the update stops and says why).
* Any other copy (GitHub's "Download ZIP"): the repository's ZIP of the newest commit is downloaded, unpacked in a
  temporary folder and its files replace the ones here, only those whose content differs. build\\, deps\\, config\\,
  userdata*\\, saves\\ and .git\\ are never written or deleted, nor is any file the repository does not have. The files
  the update wrote are listed in build\\update_manifest.json (with the commit): the next update uses it to delete files
  the repository dropped, and to leave alone files you edited yourself (--force overwrites those; the old copy goes to
  build\\update_backup\\).
* --dry-run: says what would change and writes nothing (git mode asks GitHub's API what differs).

When it ends it says whether a rebuild is needed. scripts/setup.py is resumable: `deps` and `stage` run on every start
(stage copies only the changed files into build\\work, with a new time stamp), but a step that finished earlier is skipped
unless asked, so after an update run setup.py with `--from <step>`: the compilers underneath rebuild only what is older
than its sources (by modification time). The step to name is printed.

Standard library only (git is used when the folder is a git checkout). Idea (self-update from GitHub's ZIP with a file
manifest, a commit record and a protected user area) after WindowSmash's tools/install_windowsmash.ps1 by SNO2207;
this is a separate implementation.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import ssl
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import zipfile

REPO_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_REPO = 'josiahg2502-glitch/TomodachiLife-recomp'
DEFAULT_BRANCH = 'main'
MANIFEST = os.path.join('build', 'update_manifest.json')
BACKUP = os.path.join('build', 'update_backup')
# never written, never deleted (first path component, lower case; a trailing * is a prefix)
PROTECTED = ('.git', 'build', 'deps', 'config', 'saves', 'userdata*', 'build_location.txt')   # build_location.txt: the install location chosen in the window
UA = {'User-Agent': 'tomodachi-recomp-update'}

# changed path prefix -> the earliest setup.py step that has to run again (the list is in STEPS order of setup.py)
STEPS = ['deps', 'stage', 'convert', 'modules', 'tables', 'lift', 'sdk', 'compile', 'nro', 'link', 'check']
REBUILD_RULES = [
    ('tools/extract/', 'convert'), ('tools/lifter/', 'tables'), ('tools/nro/', 'nro'), ('tools/audit/', 'check'),
    ('fighter_sdk/', 'nro'), ('mods/', 'nro'), ('tests/', 'check'), ('runtime/', 'compile'),
]
NO_REBUILD = ('docs/', 'media/', 'scripts/', 'readme', 'building.md', 'contributing.md', 'license', 'install tomodachi life.bat',
              'play tomodachi life.bat', '.gitignore', '.github/')


def say(msg=''):
    print(msg.encode('ascii', 'replace').decode('ascii'), flush=True)


def die(msg, code=1):
    say('update: ' + msg)
    sys.exit(code)


# ----------------------------------------------------------------------------------------------------- http
def http(url, accept=None, timeout=60):
    h = dict(UA)
    if accept:
        h['Accept'] = accept
    tok = os.environ.get('GITHUB_TOKEN')
    if tok and url.startswith('https://api.github.com/'):
        h['Authorization'] = 'Bearer ' + tok
    req = urllib.request.Request(url, headers=h)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.read()
    except urllib.error.URLError as e:
        reason = getattr(e, 'reason', e)
        if not (isinstance(reason, ssl.SSLError) or 'CERTIFICATE_VERIFY_FAILED' in str(e)):
            raise
    # Python's certificate check failed (issue #13: a root Windows had not fetched yet): Windows' curl.exe checks with
    # Windows, which fetches it. (No token on its command line.)
    curl = os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'curl.exe')
    if not os.path.isfile(curl):
        curl = shutil.which('curl') or curl
    cmd = [curl, '-L', '--fail', '--silent', '--show-error', '--max-time', str(timeout)]
    for k, v in h.items():
        if k != 'Authorization':
            cmd += ['-H', '%s: %s' % (k, v)]
    r = subprocess.run(cmd + [url], capture_output=True)
    if r.returncode:
        raise urllib.error.URLError('curl exit %d: %s' % (r.returncode, r.stderr.decode('utf-8', 'replace').strip()))
    return r.stdout


def api_json(url):
    return json.loads(http(url, 'application/vnd.github+json').decode('utf-8'))


def remote_sha(repo, branch):
    """The commit at the tip of the branch: GitHub's API, else `git ls-remote`."""
    try:
        s = http('https://api.github.com/repos/%s/commits/%s' % (repo, branch), 'application/vnd.github.sha').decode().strip()
        if re.fullmatch(r'[0-9a-f]{40}', s):
            return s
    except (urllib.error.URLError, OSError):
        pass
    if shutil.which('git'):
        try:                                     # no prompt for a password (it would hang), no endless wait
            r = subprocess.run(['git', 'ls-remote', 'https://github.com/%s.git' % repo, 'refs/heads/' + branch], capture_output=True,
                               text=True, timeout=60, stdin=subprocess.DEVNULL, env=dict(os.environ, GIT_TERMINAL_PROMPT='0'))
        except (OSError, subprocess.TimeoutExpired):
            return None
        m = re.match(r'([0-9a-f]{40})\s', r.stdout or '')
        if m:
            return m.group(1)
    return None


# ------------------------------------------------------------------------------------------- rebuild advice
def rebuild_advice(paths):
    """(summary lines) for a list of changed repo paths."""
    first = None
    other = 0
    for p in paths:
        low = p.replace('\\', '/').lower()
        for pre, step in REBUILD_RULES:
            if low.startswith(pre):
                if first is None or STEPS.index(step) < STEPS.index(first):
                    first = step
                break
        else:
            if not low.startswith(NO_REBUILD):
                other += 1
    out = []
    if not paths:
        out.append('Nothing changed: no rebuild.')
    elif first is None:
        out.append('Only scripts, documents or media changed: no rebuild is needed (new scripts act from the next run).')
    else:
        dump = os.path.join('build', 'dump')
        out.append('A rebuild is needed (resumable; the compilers redo only what is older than its sources):')
        out.append('    python scripts\\setup.py --dump %s --from %s' % (dump if os.path.isdir(os.path.join(REPO_DIR, dump)) else '<your dump folder>', first))
        out.append('  `stage` copies the changed sources into build\\work (always, cheap); `--from %s` then repeats %s and the later steps,'
                   % (first, first))
        out.append('  and each of those recompiles / relifts only what its inputs changed (modification time). Steps before %s stay done.' % first)
        out.append('  (or double-click Install Tomodachi Life.bat again: it does the same.)')
    if other:
        out.append('  %d changed file(s) are in no known build folder; if in doubt use --from stage.' % other)
    return out


# --------------------------------------------------------------------------------------------------- git mode
def git(*args, check=False):
    r = subprocess.run(['git', '-C', REPO_DIR] + list(args), capture_output=True, text=True, encoding='utf-8', errors='replace')
    if check and r.returncode:
        die('git %s failed: %s' % (' '.join(args), (r.stderr or r.stdout).strip()))
    return r


def norm_url(u):
    m = re.search(r'github\.com[:/]+([^/]+)/([^/]+?)(?:\.git)?/?$', u.strip(), re.I)
    return (m.group(1) + '/' + m.group(2)).lower() if m else None


def update_git(a):
    repo, branch = a.repo, a.branch
    cur = git('rev-parse', '--abbrev-ref', 'HEAD').stdout.strip()
    if cur != branch:
        die('this git checkout is on branch "%s", not "%s": nothing done (switch with git, or pass --branch).' % (cur, branch))
    head = git('rev-parse', 'HEAD', check=True).stdout.strip()
    remote = None
    for line in git('remote', '-v').stdout.splitlines():
        parts = line.split()
        if len(parts) >= 2 and norm_url(parts[1]) == repo.lower():
            remote = parts[0]
            break
    src = remote or 'https://github.com/%s.git' % repo
    say('git checkout at %s on %s; source: %s' % (head[:10], cur, remote and ('remote "%s"' % remote) or src))
    dirty = [l[3:].strip().strip('"') for l in git('status', '--porcelain', '--untracked-files=no').stdout.splitlines()]
    sha = remote_sha(repo, branch)
    if a.dry_run:
        if not sha:
            die('cannot reach GitHub (API and git both failed); nothing to compare.')
        say('GitHub %s/%s is at %s' % (repo, branch, sha[:10]))
        if sha == head:
            say('Already up to date.')
            return
        try:
            cmp = api_json('https://api.github.com/repos/%s/compare/%s...%s' % (repo, head, sha))
        except (urllib.error.URLError, OSError) as e:
            say('dry run: GitHub is at another commit; the list of files is not available (%s).' % e)
            return
        st = cmp.get('status')
        files = [f['filename'] for f in cmp.get('files', [])]
        if st == 'behind':
            say('This checkout is AHEAD of GitHub (your commits are not there): an update would do nothing.')
        elif st == 'diverged':
            say('This checkout and GitHub have diverged: an update would stop (fast-forward only). Merge or rebase with git.')
        else:
            say('Would fast-forward by %s commit(s), %d file(s) change%s:' % (cmp.get('ahead_by'), len(files), ' (list cut at 300)' if len(files) >= 300 else ''))
            for f in files[:40]:
                say('   ' + f)
            if len(files) > 40:
                say('   ... %d more' % (len(files) - 40))
            clash = sorted(set(files) & set(dirty))
            if clash:
                say('BUT these files have uncommitted changes of yours and the update would be refused: ' + ', '.join(clash))
            for l in rebuild_advice(files):
                say(l)
        say('(dry run: nothing was written)')
        return
    r = git('fetch', src, branch)
    if r.returncode:
        die('git fetch failed: ' + (r.stderr or r.stdout).strip())
    target = git('rev-parse', 'FETCH_HEAD', check=True).stdout.strip()
    if target == head or git('merge-base', '--is-ancestor', target, head).returncode == 0:
        say('Already up to date (%s)%s.' % (head[:10], '' if target == head else ': this checkout has GitHub\'s commit %s and more of its own' % target[:10]))
        return
    if git('merge-base', '--is-ancestor', head, target).returncode != 0:
        die('this checkout and GitHub\'s %s have diverged (commits on both sides): nothing changed. Merge or rebase with git yourself.' % branch)
    names = [l for l in git('diff', '--name-only', head, target).stdout.splitlines() if l]
    clash = sorted(set(names) & set(dirty))
    # a "change" that is only a file copied from a newer version of this repository (a fix handed out by hand) is not the
    # user's own work: its content equals the file in one of GitHub's commits, so it is put back and the update goes on
    published = []
    for f in clash:
        local = git('hash-object', '--', f).stdout.strip()
        commits = git('log', '--format=%H', '-n', '50', target, '--', f).stdout.split()
        if local and any(git('rev-parse', '%s:%s' % (c, f)).stdout.strip() == local for c in commits):
            published.append(f)
    if published:
        git('checkout', '--', *published)
        say('these files held a version GitHub already has (no change of yours): put back before the update: ' + ', '.join(published))
        clash = [f for f in clash if f not in published]
    if clash:
        die('refused: you have uncommitted changes in files the update changes, they would be overwritten:\n    %s\n'
            'Commit or stash them (git stash), run the update, then git stash pop.' % '\n    '.join(clash))
    r = git('merge', '--ff-only', target)
    if r.returncode:
        die('fast-forward refused by git (untracked files in the way?):\n' + (r.stderr or r.stdout).strip())
    say('Updated with git: %s -> %s, %d file(s) changed.' % (head[:10], target[:10], len(names)))
    for f in names[:40]:
        say('   ' + f)
    if len(names) > 40:
        say('   ... %d more' % (len(names) - 40))
    if dirty:
        say('Your uncommitted changes in other files were kept.')
    for l in rebuild_advice(names):
        say(l)


# --------------------------------------------------------------------------------------------------- zip mode
def sha256_file(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def is_protected(rel):
    first = rel.replace('\\', '/').split('/')[0].lower()
    for p in PROTECTED:
        if (p.endswith('*') and first.startswith(p[:-1])) or first == p:
            return True
    return False


def safe_rel(rel):
    parts = rel.split('/')
    return not (rel.startswith('/') or ':' in rel or '..' in parts or '' in parts)


def read_manifest():
    try:
        with open(os.path.join(REPO_DIR, MANIFEST), encoding='utf-8') as f:
            m = json.load(f)
        if isinstance(m.get('files'), dict):
            return m
    except (OSError, ValueError):
        pass
    return None


def write_manifest(m):
    p = os.path.join(REPO_DIR, MANIFEST)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    tmp = p + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(m, f, indent=1, sort_keys=True)
    os.replace(tmp, p)


def update_zip(a):
    repo, branch = a.repo, a.branch
    old = read_manifest()
    old_files = (old or {}).get('files', {})
    old_commit = (old or {}).get('commit')
    sha = remote_sha(repo, branch)
    ref = sha or branch
    if old_commit and sha and old_commit == sha:
        say('Already up to date (%s, recorded in %s).' % (sha[:10], MANIFEST))
        return
    say('Not a git checkout: using GitHub\'s ZIP. Recorded commit: %s; GitHub %s/%s: %s'
        % ((old_commit or 'none (first update)')[:10], repo, branch, (sha or 'unknown')[:10]))
    work = tempfile.mkdtemp(prefix='tomodachi_update_')
    try:
        zp = os.path.join(work, 'src.zip')
        url = 'https://codeload.github.com/%s/zip/%s' % (repo, ref if sha else 'refs/heads/' + branch)
        say('Downloading ' + url)
        try:
            data = http(url, timeout=300)
        except (urllib.error.URLError, OSError) as e:
            die('download failed (%s); nothing was changed.' % e)
        with open(zp, 'wb') as f:
            f.write(data)
        ex = os.path.join(work, 'x')
        with zipfile.ZipFile(zp) as z:
            if z.testzip():
                die('the downloaded ZIP is damaged; nothing was changed. Run this again.')
            tops = {n.split('/')[0] for n in z.namelist()}
            if len(tops) != 1:
                die('unexpected ZIP layout; nothing was changed.')
            top = tops.pop()
            members = [i for i in z.infolist() if not i.is_dir()]
            for i in members:
                rel = i.filename[len(top) + 1:]
                if not safe_rel(rel):
                    die('unsafe path in ZIP (%s); nothing was changed.' % i.filename)
            z.extractall(ex)
        src = os.path.join(ex, top)
        if not os.path.isfile(os.path.join(src, 'scripts', 'setup.py')):
            die('the downloaded ZIP does not look like this repository; nothing was changed.')
        if not sha:
            m = re.search(r'-([0-9a-f]{40})$', top)
            sha = m.group(1) if m else top          # the ZIP's top folder name when no commit id could be read
        new_files = {}
        for root, _, files in os.walk(src):
            for f in files:
                p = os.path.join(root, f)
                new_files[os.path.relpath(p, src).replace(os.sep, '/')] = p
        write, same, skipped_edit, protected = [], 0, [], 0
        for rel, p in sorted(new_files.items()):
            if is_protected(rel):
                protected += 1
                continue
            dst = os.path.join(REPO_DIR, rel.replace('/', os.sep))
            if os.path.isfile(dst):
                nh, ch = sha256_file(p), sha256_file(dst)
                if nh == ch:
                    same += 1
                    continue
                mine_edited = rel in old_files and old_files[rel] != ch
                if mine_edited and not a.force:
                    skipped_edit.append(rel)
                    continue
                write.append((rel, p, dst, True, nh))
            else:
                write.append((rel, p, dst, False, sha256_file(p)))
        gone, kept_edit = [], []
        for rel, h in old_files.items():
            if rel in new_files or is_protected(rel) or not safe_rel(rel):
                continue
            dst = os.path.join(REPO_DIR, rel.replace('/', os.sep))
            if not os.path.isfile(dst):
                continue
            if sha256_file(dst) != h and not a.force:
                kept_edit.append(rel)
            else:
                gone.append((rel, dst))
        changed_paths = [w[0] for w in write] + [g[0] for g in gone]
        say('%d file(s) to write (%d new), %d to delete, %d identical%s.' % (
            len(write), sum(1 for w in write if not w[3]), len(gone), same,
            ', %d protected path(s) in the ZIP ignored' % protected if protected else ''))
        for rel, _, _, existed, _ in write[:40]:
            say('   %s %s' % ('M' if existed else 'A', rel))
        if len(write) > 40:
            say('   ... %d more' % (len(write) - 40))
        for rel, _ in gone[:20]:
            say('   D ' + rel)
        if skipped_edit or kept_edit:
            say('Left alone because you edited them (use --force to overwrite, a copy goes to %s): %s' % (BACKUP, ', '.join(skipped_edit + kept_edit)))
        if not old:
            say('No %s yet: files the repository dropped earlier cannot be told from your own, none are deleted this time.' % MANIFEST)
        if a.dry_run:
            for l in rebuild_advice(changed_paths):
                say(l)
            say('(dry run: nothing was written, commit would be recorded as %s)' % sha[:10])
            return
        stamp = time.strftime('%Y%m%d_%H%M%S')
        for rel, p, dst, existed, _ in write:
            try:
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                if existed and a.force and rel in old_files:
                    b = os.path.join(REPO_DIR, BACKUP, stamp, rel.replace('/', os.sep))
                    os.makedirs(os.path.dirname(b), exist_ok=True)
                    shutil.copy2(dst, b)
                tmp = dst + '.upd_tmp'
                shutil.copyfile(p, tmp)
                os.replace(tmp, dst)          # new modification time: stage and the compilers see it as changed
            except OSError as e:
                die('could not write %s (%s). Close the game and any window using this folder, then run this again; '
                    'files already written stay, the next run finishes the rest.' % (rel, e))
        for rel, dst in gone:
            try:
                os.remove(dst)
            except OSError as e:
                say('could not delete %s (%s)' % (rel, e))
        files = {rel: sha256_file(p) for rel, p in new_files.items() if not is_protected(rel)}
        for rel in skipped_edit + kept_edit:                     # still the user's: keep the recorded hash so it stays "edited"
            if rel in old_files:
                files[rel] = old_files[rel]
        write_manifest({'repo': repo, 'branch': branch, 'commit': sha, 'zip_top': top, 'updated': time.strftime('%Y-%m-%d %H:%M:%S'), 'files': files})
        say('Updated to %s: %d file(s) written, %d deleted. build\\, deps\\, config\\, saves and your other files are untouched.' % (sha[:10], len(write), len(gone)))
        for l in rebuild_advice(changed_paths):
            say(l)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--dry-run', action='store_true', help='show what would change, write nothing')
    ap.add_argument('--repo', default=DEFAULT_REPO, help='owner/name on GitHub (default %s)' % DEFAULT_REPO)
    ap.add_argument('--branch', default=DEFAULT_BRANCH)
    ap.add_argument('--force', action='store_true', help='ZIP mode: also overwrite/delete files you edited yourself (backed up first)')
    a = ap.parse_args()
    if not re.fullmatch(r'[\w.-]+/[\w.-]+', a.repo):
        die('--repo must look like owner/name')
    if os.path.exists(os.path.join(REPO_DIR, '.git')) and shutil.which('git'):
        update_git(a)
    else:
        update_zip(a)
    return 0


if __name__ == '__main__':
    sys.exit(main())
