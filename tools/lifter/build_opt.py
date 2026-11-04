"""Optimised, side-by-side build of the lifted code (phase P3, plan\\phases\\P3_performance.md).

    python build_opt.py main [jobs]              gen_eh_o2\\*.c -> obj_eh_o2\\*.obj -> ssbu_game_eh_o2.dll (+ .lib)
    python build_opt.py nro [jobs] [module ...]  lift (if needed) + compile + link the script modules into nro_bin_eh_o2\\
    python build_opt.py one <file.c> [limit]     compile one file of the main set, print time and the fallback used
    python build_opt.py sizes <dir>              the largest functions of a generated folder

It does not replace build.py / build_nro.py: it imports them and swaps only the function that runs the compiler,
so dependency checks, linking and names stay theirs. Folders and names come from the same environment variables
(defaults here: RECOMP_GEN=gen_eh_o2 RECOMP_OBJ=obj_eh_o2 RECOMP_DLL=ssbu_game_eh_o2.dll RECOMP_NRO_SUFFIX=_eh_o2).
Run the result with:  RECOMP_GEN=gen_eh_o2 RECOMP_OBJ=obj_eh_o2 RECOMP_DLL=ssbu_game_eh_o2.dll RECOMP_EXE=<exe> python build.py link
                      SSBU_NRO_BIN=nro_bin_eh_o2 <exe> ...

Why a file can take hours at -O1/-O2: a few lifted functions have tens of thousands of instructions (the largest
215,493) in one C function with thousands of labels; several LLVM passes are worse than linear in that. Rule used
here, automatic and bounded in time:
  1. every function longer than OPTNONE_LINES statements is compiled unoptimised (`optnone`), the rest of its file at
     the chosen level; the attribute is given through a generated header (-include), the generated C is untouched;
  2. if a file still exceeds TIME_LIMIT seconds, it is retried with the limit divided by 4, then by 16;
  3. if that still exceeds the time limit, the whole file is compiled at -O0.
Every fallback is printed and written to <obj dir>\\_opt_report.txt.

Flags: -fno-strict-aliasing as in the -O0 build; -ffp-contract=off (no fused multiply-add can be formed: the -O0 build
never formed one either; fused operations the game asks for are explicit fma() calls); no fast-math; the default x86-64
target (SSE2), so no FMA / AVX instructions are selected.
"""
import collections
import hashlib
import json
import os
import re
import subprocess
import signal
import sys
import threading
import time

os.environ.setdefault('RECOMP_GEN', 'gen_eh_o2')
os.environ.setdefault('RECOMP_OBJ', 'obj_eh_o1')          # P3 (2026-10-09): the set that is the default of build.py (compiled at OPT; the name is older)
os.environ.setdefault('RECOMP_DLL', 'tomodachi_game.dll')     # build.py's name
os.environ.setdefault('RECOMP_NRO_SUFFIX', '_eh_o2')

import build          # noqa: E402  (reads the environment at import)
import build_nro      # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
CLANG = os.environ.get('RECOMP_CLANG', r'C:\llvm-9.0.0\bin\clang.exe')
# -O2 since 2026-10-26: the lifted Mii voice synthesis, which the main thread waits for, takes half the time of -O1 (its
# longest bursts 160-315 -> 70-125 ms); the whole game compiles in 23 min with 9 jobs (16 threads, 16 GB)
OPT = os.environ.get('RECOMP_OPT', '-O2')
EXTRA = ['-ffp-contract=off'] + os.environ.get('RECOMP_CFLAGS', '').split()
# RECOMP_INLINE=1: the helpers of runtime.h / neon.h / fpexact.h (every guest load / store, flag, NEON op) inlined into the
# lifted code. clang 9 at -O1 runs only the always-inline inliner, so without this each helper is a real call: measured
# 2026-10-13 (private round 15): main thread -25-30 %, audio update 9-17 -> 2-4 ms, per-frame hash identical. Same
# sources, same floating-point flags; a file whose inlined compile fails is compiled again without it (compile_file).
INLINE_FLAG = '-Dinline=__inline__ __attribute__((always_inline))'
INLINE = os.environ.get('RECOMP_INLINE', '0') not in ('', '0')      # the game code only (cmd_main); the script modules keep EXTRA
INLINE_LIMIT = float(os.environ.get('RECOMP_INLINE_LIMIT', '20'))   # seconds an inlined compile may take; longer: the file is compiled without
OPTNONE_LINES = int(os.environ.get('RECOMP_OPTNONE_LINES', '20000'))
TIME_LIMIT = float(os.environ.get('RECOMP_TIME_LIMIT', '180'))
# Linux build: address-space limit per compiler, bytes (0: none). Set by limit_memory(): the most one compiler may get.
MEM_LIMIT = 0
# Linux and macOS builds (RECOMP_TARGET linux / macos): the RAM all compilers share, bytes (0: no sharing, as on Windows).
# Each compile reserves an estimate of what it needs before it starts (mem_estimate) and gives it back when it ends, so
# the job count can follow the CPU threads while the few files that need many GB run with fewer beside them. On Linux the
# reservation is also the compiler's address-space limit: a file that passes it is compiled again with twice as much (up
# to MEM_BUDGET) before it takes the next step of the fallback ladder. macOS enforces no limit: there it only paces.
MEM_BUDGET = 0
POSIX_TARGET = os.environ.get('RECOMP_TARGET') in ('linux', 'macos')
DEF = re.compile(r'^(?:static )?void (\w+)\((Ctx \*c(?:, uint64_t rp)?)\) \{$')


def function_sizes(path):
    """[(statements, name, parameters)] for every function defined in a generated C file."""
    out = []
    cur = None
    n = 0
    with open(path, encoding='latin-1') as f:
        for line in f:
            if cur is None:
                m = DEF.match(line.rstrip('\n'))
                if m:
                    cur = (m.group(1), m.group(2))
                    n = 0
            elif line.startswith('}'):
                out.append((n, cur[0], cur[1]))
                cur = None
            else:
                n += 1
    return out


def optnone_header(src, obj, limit):
    """Write <obj>.optnone.h naming the functions of src longer than `limit` statements; returns (path, count, statements)."""
    big = [(n, name, params) for n, name, params in function_sizes(src) if n > limit]
    path = obj[:-4] + '.optnone.h'
    if not big:
        if os.path.exists(path):
            os.remove(path)
        return None, 0, 0
    with open(path, 'w') as f:
        f.write('/* generated by build_opt.py: functions too long to optimise in bounded time */\n#include "runtime.h"\n')
        for n, name, params in big:
            f.write('void %s(%s) __attribute__((optnone, noinline));\n' % (name, params))
    return path, len(big), sum(b[0] for b in big)


def ram_bytes():
    """This machine's RAM in bytes (Linux: /proc/meminfo, macOS: hw.memsize), or 0 when unknown."""
    try:
        if sys.platform == 'darwin':
            return int(subprocess.run(['sysctl', '-n', 'hw.memsize'], capture_output=True, text=True, errors='replace').stdout.strip())
        with open('/proc/meminfo') as f:
            return next(int(l.split()[1]) for l in f if l.startswith('MemTotal:')) << 10
    except (OSError, StopIteration, ValueError):
        return 0


def limit_memory(jobs):
    """Linux and macOS builds: the compilers share RAM - 4 GB (MEM_BUDGET; at least 2 GB). With current clang a lifted file
    can pass 20 GB before its time limit runs out (c_00a3.c: 12 GB after 30 s), which gets processes killed, not slowed
    down; but most files need far less (clang 9 -O1, measured 2026-10-20: 0.13 GB for the median file, 0.9 GB for the
    largest ones, 1.5 GB for c_013a.c), so a fixed share per job ((RAM - 4 GB) / jobs, the rule before) held a 16 GB
    machine to 3 compilers. Linux: a compiler over its reservation fails and is compiled again with twice as much; over the
    whole budget it takes the next step of the fallback ladder, as on a timeout."""
    global MEM_LIMIT, MEM_BUDGET
    if not POSIX_TARGET:
        return
    ram = ram_bytes()
    if not ram:
        return
    MEM_BUDGET = max(2 << 30, ram - (4 << 30))
    if os.environ.get('RECOMP_TARGET') == 'linux':        # macOS: no limit (no prlimit; RLIMIT_AS is not enforced there)
        MEM_LIMIT = MEM_BUDGET
    print('compilers share %.1f GB of RAM (%s); at most %d at once' % (MEM_BUDGET / 2.0 ** 30, 'each held to its reservation'
          if MEM_LIMIT else 'reservations pace them', jobs), flush=True)


MEM_FLOOR = [1536 << 20]                 # the smallest reservation; raised when a file reserved at the floor ran out


def mem_estimate(src, sizes):
    """What one compile of src is expected to need, bytes (a reservation, not a promise): MEM_FLOOR (1.5 GB to begin with)
    at least, 0.75 GB per MB of source, 40 KB per statement of its longest function (the giant functions drive clang's
    memory even when they are compiled unoptimised: c_00a3.c, 215,493 statements). At most MEM_BUDGET."""
    longest = max([n for n, _, _ in sizes] or [0])
    return min(MEM_BUDGET, max(MEM_FLOOR[0], os.path.getsize(src) * 768, longest * (40 << 10)))


def mem_floor_raise(want):
    """Linux: a compile reserved at the floor ran out: this clang needs more for an ordinary file than the floor assumes,
    so the next files start a quarter higher (instead of each running out once)."""
    with _mem_cv:
        if want == MEM_FLOOR[0]:             # (not a large file's own estimate; not already raised by another compile)
            MEM_FLOOR[0] = min(MEM_BUDGET, want + want // 4)


_mem_cv = threading.Condition()
_mem_used = 0
_mem_queue = collections.deque()


def mem_acquire(n):
    """Wait until n bytes of MEM_BUDGET are free, first come first served (a large reservation is not overtaken forever)."""
    global _mem_used
    me = object()
    with _mem_cv:
        _mem_queue.append(me)
        while _mem_queue[0] is not me or _mem_used + n > MEM_BUDGET:
            _mem_cv.wait()
        _mem_queue.popleft()
        _mem_used += n
        _mem_cv.notify_all()


def mem_release(n):
    global _mem_used
    with _mem_cv:
        _mem_used -= n
        _mem_cv.notify_all()


def out_of_memory(code, out):
    # (the whole output: clang prints "LLVM ERROR: out of memory" above a long stack dump. An abort under the limit, "exit
    # code 134", is taken as one too: Ubuntu 22.04's clang 14 aborts so on the largest files, 2026-11-03)
    return MEM_LIMIT and code and ('out of memory' in out or 'bad_alloc' in out or 'Cannot allocate memory' in out or 'exit code 134' in out or code < 0)


OOM = 'out of memory'                    # run_limited's output when the memory limit stopped the compiler


def run_limited(cmd, limit, mem=0):
    """Run a compiler with a time limit (and on Linux an address-space limit: mem, else MEM_LIMIT). Returns (exit code,
    output, seconds); the exit code is None when a limit stopped it, the output then OOM for memory and '' for time."""
    t = time.time()
    if MEM_LIMIT:
        cmd = ['prlimit', '--as=%d' % (mem or MEM_LIMIT), '--'] + cmd      # util-linux; inherited by clang's -cc1 child
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=(os.name != 'nt'))
    try:
        out, _ = p.communicate(timeout=limit)
        out = out.decode('latin-1')
        if out_of_memory(p.returncode, out):
            return None, OOM, time.time() - t
        return p.returncode, out[-2000:], time.time() - t
    except subprocess.TimeoutExpired:
        if os.name == 'nt':
            subprocess.run(['taskkill', '/PID', str(p.pid), '/T', '/F'], capture_output=True)      # this compiler and its -cc1 child only
        else:
            os.killpg(p.pid, signal.SIGKILL)                     # the clang wrapper's process group
        p.communicate()
        return None, '', time.time() - t


_ladder_lock = threading.Lock()
_ladders = {}


def ladder(objdir):
    """<obj dir>\\_ladder.json: for each file that needed the fallback ladder, the step that succeeded and a hash of what was
    compiled (source, flags, limits). The same file compiled again starts at that step, skipping the time limits it is known
    to exceed (up to 3 x TIME_LIMIT per file); the object is the one the full ladder makes."""
    with _ladder_lock:
        if objdir not in _ladders:
            p = os.path.join(objdir, '_ladder.json')
            try:
                with open(p) as f:
                    _ladders[objdir] = json.load(f)
            except (OSError, ValueError):
                _ladders[objdir] = {}
        return _ladders[objdir]


def ladder_save(objdir, name, key, step):
    with _ladder_lock:
        d = _ladders.setdefault(objdir, {})
        if d.get(name) == [key, step]:
            return
        if step:
            d[name] = [key, step]
        else:
            d.pop(name, None)
        with open(os.path.join(objdir, '_ladder.json'), 'w') as f:
            json.dump(d, f, indent=0, sort_keys=True)


_hints = {}


def hints(objdir):
    """What earlier compiles of the same file took: <obj dir>\\_times.json (this PC) on top of build_hints.json beside this
    script (measured on a full build and shipped). For each object: [sha1 of its source + flags, seconds, inlined]
    (inlined: 1 the inlined compile made the object, 0 it went over RECOMP_INLINE_LIMIT or failed). A first build then
    starts the slowest files first (by time, not size: the tail of the build was a few big files started late) and does
    not spend RECOMP_INLINE_LIMIT seconds on each of the ~400 files known to fall back (20 s x 400 / 12 jobs ~ 11 min).
    A hash that does not match (other lifter output, other flags) is ignored: then the old way (size order, one try)."""
    with _ladder_lock:
        if objdir not in _hints:
            d = {}
            for p in (os.path.join(HERE, 'build_hints.json'), os.path.join(objdir, '_times.json')):
                try:
                    with open(p) as f:
                        d.update(json.load(f))
                except (OSError, ValueError):
                    pass
            _hints[objdir] = d
        return _hints[objdir]


def hint_key(src):
    with open(src, 'rb') as f:
        return hashlib.sha1(f.read() + repr((OPT, EXTRA, INLINE)).encode()).hexdigest()


def hint_save(objdir, name, key, secs, inlined):
    d = hints(objdir)
    with _ladder_lock:
        d[name] = [key, round(secs, 1), inlined]


def hints_write(objdir):
    """Writes _times.json (the files compiled now plus what was known)."""
    d = _hints.get(objdir)
    if d:
        with open(os.path.join(objdir, '_times.json'), 'w') as f:
            json.dump(d, f, indent=0, sort_keys=True)


def compile_file(src, obj, base_flags, time_limit=None, extra=None):
    """Compile with the fallback ladder. Returns (exit code, output, seconds, description of the fallback or '')."""
    time_limit = time_limit or TIME_LIMIT
    extra = EXTRA if extra is None else extra
    total = 0.0
    objdir, name = os.path.dirname(obj), os.path.basename(obj)
    with open(src, 'rb') as f:
        key = hashlib.sha1(f.read() + repr((OPT, extra, base_flags, OPTNONE_LINES, time_limit) + ((MEM_LIMIT,) if MEM_LIMIT else ())).encode()).hexdigest()
    known = ladder(objdir).get(name)
    first = known[1] if known and known[0] == key else 0
    want = mem_estimate(src, function_sizes(src)) if MEM_BUDGET else 0      # Linux / macOS: this compile's share of RAM
    raised = ''
    tried = None
    for step, limit in enumerate((OPTNONE_LINES, OPTNONE_LINES // 4, OPTNONE_LINES // 16)):
        if step < first:
            continue
        hdr, count, lines = optnone_header(src, obj, limit)
        if step and not count:
            continue
        # Linux / macOS: as many functions unoptimised as in the attempt that just failed is the same set (each limit's set
        # holds the previous one), so the same compile, which fails the same way (c_03d9.c: one function of 77,061
        # statements and the rest under 1,250 ran three identical times into the 180 s limit)
        if POSIX_TARGET and count == tried:
            continue
        tried = count
        cmd = [CLANG, '-c', OPT] + extra + base_flags + (['-include', hdr] if hdr else []) + [src, '-o', obj]
        while True:
            if want:
                mem_acquire(want)
            try:
                code, out, secs = run_limited(cmd, time_limit, want)
            finally:
                if want:
                    mem_release(want)
            total += secs
            if code is None and out == OOM and want and want < MEM_BUDGET:     # Linux: over its reservation, not the budget
                mem_floor_raise(want)
                want = min(MEM_BUDGET, want * 2)
                raised = ' (memory reservation raised to %.1f GB)' % (want / 2.0 ** 30)
                continue
            break
        if code is not None:
            note = ''
            if count:
                note = '%d function(s) over %d statements unoptimised (%d statements)' % (count, limit, lines)
            if code == 0:
                ladder_save(objdir, name, key, step)
            return code, out, total, note + (' (step known from _ladder.json)' if first else '') + raised
    cmd = [CLANG, '-c', '-O0'] + extra + base_flags + [src, '-o', obj]
    if want:
        mem_acquire(want)
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
    finally:
        if want:
            mem_release(want)
    if r.returncode == 0:
        ladder_save(objdir, name, key, 3)
    return r.returncode, (r.stdout + r.stderr)[-2000:], total, 'WHOLE FILE at -O0 (time limit %.0fs%s exceeded three times)' % (
        time_limit, ' or memory limit %.1f GB' % (MEM_LIMIT / 2.0 ** 30) if MEM_LIMIT else '')


_tries = collections.Counter()
_tries_lock = threading.Lock()
_jobs = [0]


def compile_file_inline(src, obj, base_flags, time_limit=None):
    """With RECOMP_INLINE: ONE inlined compile with a short time limit (RECOMP_INLINE_LIMIT, 20 s); a file that takes longer
    (the biggest generated files: inlining made the whole game compile 2-4x slower, testers 2026-10-21 "40 min -> over an
    hour") or fails is compiled the normal way (compile_file, its own ladder), so the slowdown per file is bounded. The
    files that compile fast, most of them, keep the faster code. Without RECOMP_INLINE: compile_file."""
    if not INLINE:
        r = compile_file(src, obj, base_flags, time_limit)
        if r[0] == 0:
            hint_save(os.path.dirname(obj), os.path.basename(obj), hint_key(src), r[2], 0)
        return r
    with _tries_lock:
        _tries[obj] += 1
        n = _tries[obj]
    objdir, name = os.path.dirname(obj), os.path.basename(obj)
    key = hint_key(src)
    known = hints(objdir).get(name)
    if n == 1 and known and known[0] == key and known[2] == 0:
        code, out, secs, note = compile_file(src, obj, base_flags, time_limit)
        hint_save(objdir, name, key, secs, 0)
        return code, out, secs, 'helpers not inlined (known to go over %.0f s)' % INLINE_LIMIT + ('; ' + note if note else '')
    if n == 1:
        hdr, count, lines = optnone_header(src, obj, OPTNONE_LINES)
        cmd = [CLANG, '-c', OPT] + EXTRA + [INLINE_FLAG] + base_flags + (['-include', hdr] if hdr else []) + [src, '-o', obj]
        want = mem_estimate(src, function_sizes(src)) * 2 if MEM_BUDGET else 0
        if want:
            mem_acquire(want)
        try:
            # a file known to make it inlined (same source and flags) gets room for a busier PC: without it ~30 of the files
            # measured at 15-19 s went over the limit on the next full build and lost the inlined helpers
            limit = max(INLINE_LIMIT, 2 * known[1] + 5) if known and known[0] == key and known[2] == 1 else INLINE_LIMIT
            code, out, secs = run_limited(cmd, limit, want)
        finally:
            if want:
                mem_release(want)
        if code == 0:
            hint_save(objdir, name, key, secs, 1)
            return 0, out, secs, ('%d function(s) over %d statements unoptimised (%d statements)' % (count, OPTNONE_LINES, lines) if count else '')
        code2, out2, secs2, note2 = compile_file(src, obj, base_flags, time_limit)
        if code2 == 0:
            hint_save(objdir, name, key, secs2, 0)
        return code2, out2, secs + secs2, ('helpers not inlined (%s)' % ('over %.0f s' % INLINE_LIMIT if code is None else 'the inlined compile failed')) + ('; ' + note2 if note2 else '')
    return compile_file(src, obj, base_flags, time_limit)      # a retry round (build.py, fewer jobs): the normal way



_report = []


def report(objdir):
    if not _report:
        return
    with open(os.path.join(objdir, '_opt_report.txt'), 'a') as f:
        f.write('# %s  %s %s %s  optnone over %d statements, time limit %.0fs\n' % (time.strftime('%Y-%m-%d %H:%M'), CLANG, OPT, ' '.join(EXTRA), OPTNONE_LINES, TIME_LIMIT))
        f.writelines(line + '\n' for line in _report)
    del _report[:]


# ---- main: build.compile_one replaced -------------------------------------------------------------------------
def compile_one_main(name):
    src = os.path.join(build.GEN, name)
    obj = os.path.join(build.OBJ, name[:-2] + '.obj')
    code, out, secs, note = compile_file_inline(src, obj, ['-w', '-fno-strict-aliasing', '-I', HERE, '-I', build.GEN] + build.NOCRT_FLAGS + build.PLAT_FLAGS)
    if note or secs > 60:
        line = '%-12s %6.0fs  %s' % (name, secs, note)
        _report.append(line)
        print('  ' + line, flush=True)
    return name, code, out, secs


def cmd_main(jobs):
    build.compile_one = compile_one_main
    _jobs[0] = jobs
    limit_memory(jobs)
    t0 = time.time()
    # objects compiled with the other RECOMP_INLINE setting are compiled again (the compile skips an object newer than its
    # source, whatever flags made it): a build from before inlining existed gets the faster code on its next update
    stamp, want = os.path.join(build.OBJ, '_inline.txt'), '1' if INLINE else '0'
    try:
        have = open(stamp).read().strip()
    except OSError:
        have = None                          # no stamp: objects from before the stamp existed were compiled without inlining
    old = [n for n in os.listdir(build.OBJ) if n.endswith('.obj')] if os.path.isdir(build.OBJ) else []
    if old and have != want and (have is not None or want == '1'):
        print('the game objects were compiled %s the inlined helpers: all %d are compiled again (once)' % ('with' if have == '1' else 'without', len(old)), flush=True)
        for n in old:
            os.remove(os.path.join(build.OBJ, n))
    def order_key(n):                    # slowest first by the time it took before (same source and flags), else by size
        h = hints(build.OBJ).get(n[:-2] + '.obj')
        size = os.path.getsize(os.path.join(build.GEN, n))
        if h and h[0] == hint_key(os.path.join(build.GEN, n)):
            return -h[1]
        return -size * 3e-6                # ~1 MB of source ~ 3 s: unmeasured files sit among the measured ones
    build.order_key = order_key
    try:
        rc = build.cmd_compile(jobs, False)
    finally:
        hints_write(build.OBJ)
    report(build.OBJ)
    if rc:
        return rc
    with open(stamp, 'w') as f:
        f.write(want + chr(10))
    rc = build.build_game_dll()
    print('main: compile + link %.0fs' % (time.time() - t0))
    return rc


# ---- modules: build_nro.run_clang replaced --------------------------------------------------------------------
def run_clang_nro(job):
    args, label = job
    src, obj = args[-3], args[-1]
    base = [a for a in args[:-3] if a not in ('-c', '-O0', '-O1', '-O2')]
    code, out, secs, note = compile_file(src, obj, base)
    if note or secs > 60:
        line = '%-40s %6.0fs  %s' % (os.path.basename(os.path.dirname(src)) + '/' + label, secs, note)
        _report.append(line)
        print('  ' + line, flush=True)
    return label, code, out, secs


def cmd_nro(jobs, names):
    build_nro.run_clang = run_clang_nro
    limit_memory(jobs)
    if not names:
        names = [e['name'] for e in build_nro.load_map()]
    rc = build_nro.main(names + ['--jobs=%d' % jobs])
    # a compiler that crashed with many running at once usually ran out of memory (clang "due to signal"): the modules are
    # built again with far fewer at a time, then one; only the objects that failed are compiled again
    for retry in (max(1, jobs // 4), 1):
        if not rc or retry >= jobs:
            continue
        print('building the modules again with %d job(s): some compiles failed (usually out of memory with %d at once)' % (retry, jobs), flush=True)
        rc = build_nro.main(names + ['--jobs=%d' % retry])
    report(os.path.join(HERE, 'obj_nro' + os.environ['RECOMP_NRO_SUFFIX']))
    return rc


if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'main':
        sys.exit(cmd_main(int(sys.argv[2]) if len(sys.argv) > 2 else 28))
    if cmd == 'nro':
        rest = sys.argv[2:]
        jobs = int(rest.pop(0)) if rest and rest[0].isdigit() else 28
        sys.exit(cmd_nro(jobs, rest))
    if cmd == 'one':
        os.makedirs(build.OBJ, exist_ok=True)
        src = sys.argv[2] if os.path.isabs(sys.argv[2]) else os.path.join(build.GEN, sys.argv[2])
        obj = os.path.join(os.environ.get('RECOMP_ONE_OUT', build.OBJ), os.path.basename(src)[:-2] + '.obj')
        code, out, secs, note = compile_file(src, obj, ['-w', '-fno-strict-aliasing', '-I', HERE, '-I', os.path.dirname(src)], float(sys.argv[3]) if len(sys.argv) > 3 else None)
        print('%s: exit %s  %.1fs  %s  %s' % (os.path.basename(src), code, secs, note or 'fully optimised', out.strip()[:500]))
    if cmd == 'sizes':
        rows = []
        for n in sorted(os.listdir(sys.argv[2])):
            if n.startswith('c_') and n.endswith('.c'):
                rows += [(s, name, n) for s, name, _ in function_sizes(os.path.join(sys.argv[2], n))]
        rows.sort(reverse=True)
        print('functions: %d   statements: %d' % (len(rows), sum(r[0] for r in rows)))
        for limit in (1000, 2000, 4000, 8000, 16000, 32000):
            big = [r for r in rows if r[0] > limit]
            print('  over %6d statements: %5d functions, %9d statements (%.1f%%)' % (limit, len(big), sum(r[0] for r in big), 100.0 * sum(r[0] for r in big) / sum(r[0] for r in rows)))
        for r in rows[:25]:
            print('  %7d  %s  %s' % r)
