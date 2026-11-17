#!/usr/bin/env python3
"""route_one.py : ONE headless field route with the environment tools/route_batch.sh gives a run, on Linux or macOS.

  tools/route_one.py --bin ./khnmc --exe PATH/kh2fm.exe --data DATA_TREE --pkgdir PKGDIR --out OUTDIR
                     [--game-frames 7000] [--seconds 600] [--difficulty none|0..3] [--input input/route-field-battle.txt]
                     [--env VAR=VALUE]... [--require field|none] [--label TEXT] [--no-nice] [--nice 10] [--min-free-pct 30]
                     [--ios-paths]

route_batch.sh is the Linux batch driver (waves, /proc/meminfo, taskset, coreutils timeout); macOS has none of those, and the Mac
mini runs one route at a time.  This script makes the same run: fresh OUTDIR/{root,docs}, the same variables (KH_EXE KH_ROOT KH_DOCS
KHNMC_DATA KHNMC_PKGDIR KHNMC_INPUT_SCRIPT KH_RUN_SECONDS KH_DUMP_THREADS KHNMC_PHASELOG KHNMC_FILELOG KH_RUN_GAME_FRAMES
KHNMC_DIFFICULTY), `nice -n 10`, a watchdog that kills a run still alive 90 s after its own time limit (exit 124), and the same
verdict: ok only for exit 0 AND the phase log showing FIELD_COMMAND.execute calls (else DIVERGED / FAIL).  Nothing is retried.
Task 27: the default input is the state-driven route input/route-field-battle.txt.  For a ROUTE script (@wait / @press / @select /
@expect steps) --difficulty sets KHNMC_ROUTE_DIFFICULTY (the route chooses it in the game's New Game dialog) instead of
KHNMC_DIFFICULTY, a run whose route did not finish is DIVERGED (signature: the step it waited at), and the summary has `states`:
the game frame at which each step's state was met (from `kh: route report`).  A frame-numbered script behaves as before.
Outputs: OUTDIR/run.out, phase.log, files.log, exit, env, summary.tsv (one header line + one row; the columns of route_batch.sh
plus damage calls, HD assets served, source voices and the peak RSS) and summary.json with every counter that was parsed.
macOS: before starting, `memory_pressure` must report at least --min-free-pct free (default 30), else the run is refused (exit 3).
In an --env value `@OUT@` stands for OUTDIR (absolute), e.g. --env KHNMC_AUDIO=wav:@OUT@/audio.wav.
--ios-paths (task 26): BIN is a KH_IOS_PATHS build (make IOS_PATHS=1).  The run is then started the way an app is: OUTDIR/home is
the app container (Documents/kh2fm.exe, Documents/data, Documents/pkgdir as links, the input script as a copy, Documents/khnmc.conf
with every variable of the run), the process gets an EMPTY environment except PATH, KH_APP_HOME and KH_TMPDIR, and its working
directory is OUTDIR, not the game directory.  KH_EXE, KH_ROOT, KH_DOCS, KHNMC_DATA and KHNMC_PKGDIR are not passed at all: they
must come from the container defaults (runtime/host_env.c).  The log, phase log and file log are written inside Documents and
copied to OUTDIR afterwards; the status file must say `exit N` with the status the process really ended with, else the run FAILS.
Exit status: 0 ok, 1 failed or diverged, 2 usage, 3 refused.  Python 3.6+, standard library.
"""
import argparse
import json
import os
import re
import resource
import shutil
import subprocess
import sys
import time


def last(pattern, text, group=1, conv=int):
    m = None
    for m in re.finditer(pattern, text):
        pass
    return conv(m.group(group)) if m else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--bin', required=True); ap.add_argument('--exe', required=True); ap.add_argument('--data', required=True)
    ap.add_argument('--pkgdir', required=True); ap.add_argument('--out', required=True)
    ap.add_argument('--game-frames', type=int, default=7000); ap.add_argument('--seconds', type=int, default=600)
    ap.add_argument('--difficulty', default='none', choices=['none', '0', '1', '2', '3'])
    ap.add_argument('--input', default=None); ap.add_argument('--env', action='append', default=[])
    ap.add_argument('--require', default='field', choices=['field', 'none']); ap.add_argument('--label', default='')
    ap.add_argument('--no-nice', action='store_true'); ap.add_argument('--min-free-pct', type=int, default=30)
    ap.add_argument('--nice', type=int, default=10); ap.add_argument('--ios-paths', action='store_true')
    a = ap.parse_args()
    if not 0 <= a.nice <= 20:
        sys.stderr.write('route_one: --nice takes 0..20\n'); return 2
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    inp = a.input or os.path.join(here, 'input', 'route-field-battle.txt')
    for f in (a.bin, a.exe, a.data, a.pkgdir, inp):
        if not os.path.exists(f):
            sys.stderr.write('route_one: missing %s\n' % f); return 2
    darwin = sys.platform == 'darwin'
    if darwin:
        try:
            mp = subprocess.check_output(['memory_pressure'], universal_newlines=True).strip().splitlines()[-1]
            free = int(re.search(r'(\d+)%', mp).group(1))
        except Exception as e:                                  # cannot tell: do not start a heavy step blind
            sys.stderr.write('route_one: cannot read memory_pressure (%s): not starting\n' % e); return 3
        if free < a.min_free_pct:
            sys.stderr.write('route_one: memory free %d%% is below %d%%: not starting\n' % (free, a.min_free_pct)); return 3
        print('route_one: %s' % mp)
    out = os.path.abspath(a.out)
    for sub in ('root', 'docs'):
        shutil.rmtree(os.path.join(out, sub), ignore_errors=True)
    home = os.path.join(out, 'home'); docs = os.path.join(home, 'Documents'); mark = os.path.join(home, '.khnmc-ios-paths-home')
    if os.path.exists(home):
        if not os.path.exists(mark):
            sys.stderr.write('route_one: %s exists and is not from this script: not touching it\n' % home); return 2
        shutil.rmtree(home)
    if a.ios_paths:
        os.makedirs(docs); os.makedirs(os.path.join(home, 'tmp')); open(mark, 'w').close()
    else:
        os.makedirs(os.path.join(out, 'root')); os.makedirs(os.path.join(out, 'docs'))
    for f in ('run.out', 'launch.out', 'phase.log', 'files.log', 'exit', 'summary.tsv', 'summary.json'):
        try: os.unlink(os.path.join(out, f))
        except OSError: pass
    binc = os.path.join(out, 'khnmc.bin'); shutil.copy2(a.bin, binc)      # rebuilding during the run cannot change what runs
    env = dict(os.environ)
    run_env = {'KH_EXE': os.path.abspath(a.exe), 'KH_ROOT': os.path.join(out, 'root'), 'KH_DOCS': os.path.join(out, 'docs'),
               'KHNMC_DATA': os.path.abspath(a.data), 'KHNMC_PKGDIR': os.path.abspath(a.pkgdir), 'KHNMC_INPUT_SCRIPT': os.path.abspath(inp),
               'KH_RUN_SECONDS': str(a.seconds), 'KH_DUMP_THREADS': '1', 'KHNMC_PHASELOG': os.path.join(out, 'phase.log'),
               'KHNMC_FILELOG': os.path.join(out, 'files.log')}
    route = bool(re.search(r'^[ \t]*@(wait|press|select|expect)(\s|$)', open(inp, errors='replace').read(), re.M))
    if a.difficulty != 'none': run_env['KHNMC_ROUTE_DIFFICULTY' if route else 'KHNMC_DIFFICULTY'] = a.difficulty
    if a.game_frames > 0: run_env['KH_RUN_GAME_FRAMES'] = str(a.game_frames)
    for kv in a.env:
        if '=' not in kv:
            sys.stderr.write('route_one: --env takes VAR=VALUE\n'); return 2
        k, v = kv.split('=', 1); run_env[k] = v.replace('@OUT@', out)
    wrapper = '' if a.no_nice else 'nice -n %d' % a.nice
    cmd = ([] if a.no_nice else ['nice', '-n', str(a.nice)]) + [binc]
    if a.ios_paths:
        # the app container: what the user would put into Documents, and a conf that carries the whole run
        os.symlink(os.path.abspath(a.exe), os.path.join(docs, 'kh2fm.exe')); os.symlink(os.path.abspath(a.data), os.path.join(docs, 'data'))
        os.symlink(os.path.abspath(a.pkgdir), os.path.join(docs, 'pkgdir')); shutil.copy(inp, os.path.join(docs, 'input.txt'))
        for k in ('KH_EXE', 'KH_ROOT', 'KH_DOCS', 'KHNMC_DATA', 'KHNMC_PKGDIR'): del run_env[k]
        run_env['KHNMC_INPUT_SCRIPT'] = '@DOCS@/input.txt'; run_env['KHNMC_PHASELOG'] = '@DOCS@/phase.log'; run_env['KHNMC_FILELOG'] = '@DOCS@/files.log'
        with open(os.path.join(docs, 'khnmc.conf'), 'w') as f:
            f.write('# written by tools/route_one.py --ios-paths: one headless field route, the way the app shell would be told to run it\n'
                    'log khnmc.log\nstatus khnmc.status\n' + ''.join('env %s=%s\n' % kv for kv in run_env.items()))
        env = {'PATH': '/usr/bin:/bin', 'KH_APP_HOME': home, 'KH_TMPDIR': os.path.join(home, 'tmp')}
        cwd, first = out, 'launch.out'
        with open(os.path.join(out, 'env'), 'w') as f:
            f.write('KH_APP_HOME=%s\nKH_TMPDIR=%s\nPATH=/usr/bin:/bin\n(nothing else in the environment; Documents/khnmc.conf:)\n' % (home, os.path.join(home, 'tmp'))
                    + open(os.path.join(docs, 'khnmc.conf')).read() + 'wrapper=%s\n' % wrapper)
    else:
        env.update(run_env); cwd, first = os.path.join(out, 'root'), 'run.out'
        with open(os.path.join(out, 'env'), 'w') as f:
            f.write(''.join('%s=%s\n' % kv for kv in run_env.items()) + 'wrapper=%s\n' % wrapper)
    t0 = time.time(); status = None
    with open(os.path.join(out, first), 'wb') as ro:
        p = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=ro, stderr=subprocess.STDOUT)
        try:
            p.wait(timeout=a.seconds + 90); status = p.returncode
        except subprocess.TimeoutExpired:                       # still alive 90 s after its own time limit: a hang
            p.kill(); p.wait(); status = 124
    wall = time.time() - t0
    ios_note = ''
    if a.ios_paths:
        # the logs were written inside the container: bring them to where every other run has them
        with open(os.path.join(out, 'run.out'), 'wb') as ro:
            for src in (os.path.join(out, 'launch.out'), os.path.join(docs, 'khnmc.log')):
                if os.path.exists(src): ro.write(open(src, 'rb').read())
        for name in ('phase.log', 'files.log'):
            if os.path.exists(os.path.join(docs, name)): shutil.copy(os.path.join(docs, name), os.path.join(out, name))
        try: st = open(os.path.join(docs, 'khnmc.status')).read().strip()
        except OSError: st = '<no status file>'
        real = status if status >= 0 else 128 - status
        left = sorted(os.listdir(os.path.join(home, 'tmp')))
        stray = sorted(x for x in os.listdir(out) if x in ('docs', 'root', 'data'))
        if st != 'exit %d' % real: ios_note = 'status file says %r, the process ended with %d' % (st, real)
        elif stray: ios_note = 'the launch directory got %s: a path that did not come from the container' % ' '.join(stray)
        print('route_one: ios-paths: status file %r (process exit %d); left in the scratch directory: %s; container: %s'
              % (st, real, ' '.join(left) or 'nothing', ' '.join(sorted(os.listdir(docs)))))
    ru = resource.getrusage(resource.RUSAGE_CHILDREN)
    rss_mb = ru.ru_maxrss // (1 << 20) if darwin else ru.ru_maxrss // 1024      # bytes on macOS, KiB on Linux
    ex = status if status >= 0 else 128 - status
    with open(os.path.join(out, 'exit'), 'w') as f: f.write('%d\n' % ex)
    txt = open(os.path.join(out, 'run.out'), 'r', errors='replace').read()
    def rd(name):
        try: return open(os.path.join(out, name), 'r', errors='replace').read()
        except OSError: return ''
    files, phase = rd('files.log'), rd('phase.log')
    gl = [l for l in txt.splitlines() if 'kh: gfx: draws=' in l and 'gfx frame' not in l]
    gl = gl[-1] if gl else ([l for l in txt.splitlines() if 'gfx frame=' in l] or [''])[-1]
    num = lambda name: last(r'[ =:]%s=(\d+)' % name, ' ' + gl)
    c = {'label': a.label, 'exit': ex, 'difficulty': a.difficulty,
         'presents': num('presents'), 'draws': num('draws'), 'psos': num('psos'), 'resources': num('resources'), 'clears': num('clears'),
         'gframes': last(r'kh: input report:.* game-frames=(\d+)', txt),
         'file_hits': len(re.findall(r'^HIT ', files, re.M)), 'file_miss': len(re.findall(r'^MISS ', files, re.M)),
         'field_cmds': sum(int(x) for x in re.findall(r'FIELD_COMMAND\.execute=(\d+)', phase)),
         'late': last(r'scripted XInput: hold at script frame.*script delayed by (\d+) presents', txt),
         'damage_calls': sum(int(x) for x in re.findall(r'kh: difficulty damage slot=\d+ calls=(\d+)', txt)),
         'damage_sum': sum(int(x) for x in re.findall(r'kh: difficulty damage slot=\d+ calls=\d+ zero=\d+ result_sum_u32=(\d+)', txt)),
         'hd_assets': last(r'kh: hd: .*assets-served=(\d+)', txt), 'hd_bytes': last(r'assets-served=\d+ \((\d+) bytes\)', txt),
         'source_voices': last(r'voices created: source=(\d+)', txt), 'threads': last(r'kh: threads: (\d+) created', txt),
         'fiber_switches': last(r'fiber-switches=(\d+)', txt), 'import_calls': last(r'process exit\(\d+\) after (\d+) import calls', txt),
         'id_slots': last(r'kh: object-id table: (\d+) of 64', txt),
         'id_zone_mib': last(r'guest arena: id zone .*high-water 0x[0-9a-f]+ = (\d+) MiB', txt),
         'far_zone_mib': last(r'guest arena: far zone .*high-water 0x[0-9a-f]+ = (\d+) MiB', txt),
         'wall_s': int(wall), 'cpu_s': int(ru.ru_utime + ru.ru_stime), 'rss_mb': rss_mb, 'route': route,
         'states': (lambda m: m.group(1).strip().replace(' ', ',') if m else None)(re.search(r'kh: route report:.*met at game frame:([^;\n]*)', txt)),
         'binary_sha256': subprocess.check_output(['shasum', '-a', '256', binc] if darwin else ['sha256sum', binc], universal_newlines=True)[:16]}
    result, sig = 'ok', ''
    if ios_note:
        result, sig = 'FAIL', 'ios-paths: ' + ios_note
    elif ex == 0 and a.require == 'field' and route and (not re.search(r'kh: route report:', txt) or re.search(r'kh: route report:.*ROUTE INCOMPLETE', txt)):
        result = 'DIVERGED'
        m = re.search(r'kh: route report:.*; (waiting at line .*)', txt)
        sig = 'exit 0 but the route did not finish | %s' % (m.group(1) if m else 'no route report line in run.out')
    elif ex == 0 and a.require == 'field' and c['field_cmds'] == 0:
        result = 'DIVERGED'
        ls = re.findall(r'kh: (InitialSetting frame=.*)', txt)
        sig = 'exit 0 but the route never reached the field | last %s' % (ls[-1] if ls else '-')
    elif ex != 0:
        result = 'FAIL'
        m = re.search(r'^(kh: SIGNAL .*|kh: FATAL: .*|kh_fault: .*)$', txt, re.M)
        line = m.group(1) if m else 'no report line in run.out'
        if ex == 124: line = 'TIMEOUT (watchdog: still running %d s after start)' % (a.seconds + 90)
        if ex == 137: line = 'KILLED (SIGKILL: watchdog or out of memory)'
        bt = txt.split('---- host backtrace ----', 1)[1] if '---- host backtrace ----' in txt else ''
        frames = re.findall(r'\b(?:orig_|__real_)?f_[0-9a-f]+\b(?:\s*\+\s*(?:0x)?[0-9a-f]+)?', bt)[:4]      # glibc "(f_x+0x12)" and macOS "f_x + 18"
        g = re.search(r'rip\(state\)=[0-9a-f]+', txt)
        sig = '%s | %s | %s' % (line, g.group(0) if g else '', '<'.join(re.sub(r'\s+', '', f) for f in frames) or 'no lifted frame')
    c['result'] = result; c['signature'] = sig
    cols = ['label', 'exit', 'result', 'presents', 'gframes', 'draws', 'psos', 'resources', 'file_hits', 'file_miss', 'field_cmds', 'late',
            'damage_calls', 'hd_assets', 'source_voices', 'id_slots', 'wall_s', 'cpu_s', 'rss_mb', 'states', 'signature']
    row = ['-' if c.get(k) is None else str(c.get(k)) for k in cols]
    with open(os.path.join(out, 'summary.tsv'), 'w') as f: f.write('\t'.join(cols) + '\n' + '\t'.join(row) + '\n')
    with open(os.path.join(out, 'summary.json'), 'w') as f: json.dump(c, f, indent=1, sort_keys=True)
    w = [max(len(x), len(y)) for x, y in zip(cols, row)]
    print('  '.join(x.ljust(n) for x, n in zip(cols, w))); print('  '.join(x.ljust(n) for x, n in zip(row, w)))
    if result == 'ok':
        try: os.unlink(binc)
        except OSError: pass
        return 0
    return 1


if __name__ == '__main__':
    sys.exit(main())
