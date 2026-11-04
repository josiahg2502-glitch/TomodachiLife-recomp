"""Compile the lifted main (recomp_eh.py output) to objects, as ssbu-recomp's build.py compiles its game DLL.

    python compile_main.py <gen dir> <obj dir> [jobs]

clang 9 -O1 per file; a file still compiling after TIMEOUT seconds is compiled at -O0 instead (build.py keeps a fixed
list of such files for SSBU, this finds them). Objects newer than their source and the headers are kept. Writes
<obj dir>/compile_report.json: per file the level used, the seconds and the error text of a failure.
"""
import concurrent.futures
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
RUNTIME = os.path.join(HERE, '..', '..', 'runtime')
CLANG = os.environ.get('RECOMP_CLANG', os.path.join(HERE, '..', '..', '..', 'ssbu-recomp', 'deps', 'llvm', 'bin', 'clang.exe'))
TIMEOUT = int(os.environ.get('COMPILE_TIMEOUT', '900'))
HEADERS = ['runtime.h', 'neon.h', 'fpexact.h', 'eh_runtime.h']


def compile_one(gen, obj_dir, name):
    src, obj = os.path.join(gen, name), os.path.join(obj_dir, name[:-2] + '.obj')
    t = time.time()
    for level in ('-O1', '-O0'):
        cmd = [CLANG, '-c', level, '-w', '-fno-strict-aliasing', '-I', RUNTIME, '-I', gen, src, '-o', obj]
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=TIMEOUT if level == '-O1' else None)
        except subprocess.TimeoutExpired:
            continue
        return name, level, r.returncode, r.stderr[-3000:], time.time() - t
    return name, '-O0', 1, 'timed out', time.time() - t


def main():
    gen, obj_dir = sys.argv[1], sys.argv[2]
    jobs = int(sys.argv[3]) if len(sys.argv) > 3 else os.cpu_count()
    os.makedirs(obj_dir, exist_ok=True)
    newest = max(os.path.getmtime(os.path.join(RUNTIME, h)) for h in HEADERS)
    sources = sorted(n for n in os.listdir(gen) if n.endswith('.c'))
    for n in os.listdir(obj_dir):                  # objects of files a new lift no longer writes: they would be linked
        if n.endswith('.obj') and n[:-4] + '.c' not in sources:
            os.remove(os.path.join(obj_dir, n))
    todo = [n for n in sources if not os.path.exists(os.path.join(obj_dir, n[:-2] + '.obj'))
            or os.path.getmtime(os.path.join(obj_dir, n[:-2] + '.obj')) < max(newest, os.path.getmtime(os.path.join(gen, n)))]
    todo.sort(key=lambda n: -os.path.getsize(os.path.join(gen, n)))          # biggest first: they set the total time
    print('sources: %d   to compile: %d   jobs: %d' % (len(sources), len(todo), jobs), flush=True)
    report, failed, t0 = {}, [], time.time()
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        futures = [pool.submit(compile_one, gen, obj_dir, n) for n in todo]
        for i, f in enumerate(concurrent.futures.as_completed(futures), 1):
            name, level, code, err, secs = f.result()
            report[name] = {'level': level, 'seconds': round(secs, 1), 'error': err if code else ''}
            if code:
                failed.append(name)
                print('FAILED %s (%s, exit %s)\n%s' % (name, level, code, err), flush=True)
            if i % 50 == 0 or i == len(todo):
                print('  %d/%d   %.0fs' % (i, len(todo), time.time() - t0), flush=True)
    with open(os.path.join(obj_dir, 'compile_report.json'), 'w') as f:
        json.dump(report, f, indent=1)
    o0 = sorted(n for n, r in report.items() if r['level'] == '-O0')
    print('compiled: %d   failed: %d   at -O0 after the time limit: %d %s   elapsed: %.0fs'
          % (len(todo) - len(failed), len(failed), len(o0), o0, time.time() - t0))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
