#!/usr/bin/env python3
"""movie_run_check.py (task 44, review): does a run's log show that its pre-rendered movies ended BY THEMSELVES?

  tools/movie_run_check.py RUN.OUT [--first NAME] [--second NAME] [--movies N] [--nopause yes|no] [--pictures MADE,REUSED]
                                   [--steady-resources] [--mode end|fail-wait|fail-skip] [--expect-fatal NAME]
  tools/movie_run_check.py --selftest

Reads the lines of the movie seam (runtime/null_mf.c):
  kh: movie: #N <name>.mp4 requested at game frame F (...)
  kh: movie: #N <name>.mp4: the event ended by itself at game frame G, ...      (the natural-end site of 0x2cf980)
  kh: movie: #N <name>.mp4: the event was ended by SKIP ...                     (not by itself)
  kh: movie: #N <name>.mp4: the event was ended ... by a caller of 0x2cbd80 ...  (not as a movie ends)
  kh: movie: REQUEST DROPPED BY THE GAME ...                                    (the game's guard: nothing was queued)
  kh: movie: #N <name>.mp4: player torn down: reader released at game frame H ...
  kh: movies: mode end: ...                                                     (the exit line)
Default (--mode end): every request has its `ended by itself` line and its `player torn down` line, in that order, no request was
dropped, skipped or ended another way, the exit line is there and does not say `NOT every`, no `kh: FATAL` and no `kh: SIGNAL`.
  --first NAME          the first request is NAME.mp4
  --second NAME         the second request is NAME.mp4 (two movies in one session)
  --movies N            exactly N requests
  --nopause yes|no      the first request's line says the scene has NO pause screen (yes) / does not say so (no)
  --pictures M,R        the exit line says M pictures made by the engine and R times the kept one handed out again
  --steady-resources    from the second movie on, the `resources=` counter of the `kh: gfx frame=` lines (D3D12 resources created so
                        far) is the same 5 presents after the player's teardown line as at the last present before the request
                        line: a movie makes no new resource (the first one makes the picture).  The presents are found by the
                        ORDER of the lines in the log: a game frame number is not a present number (the first version of this
                        check took one for the other and compared two presents BEFORE the request: it could not fail)
  --mode fail-wait      KHNMC_MOVIES=fail and nothing skips: the first request is there, says KHNMC_MOVIES=fail, and its event did
                        NOT end before the run did (the old black wait); with --nopause yes the line must say FOR EVER
  --mode fail-skip      KHNMC_MOVIES=fail and the route skips: every request's event was ended by SKIP
  --expect-fatal NAME   the run MUST end with exactly one `kh: FATAL: NAME: ...` (and no signal): the way a route shows the old
                        wait (`route-wait-timeout`: the state after the movie never came).  Without it any fatal or signal fails
Prints one line per movie and a verdict; exit status 0 = all checks hold, 1 = not, 2 = usage.  Standard library."""
import re, sys

REQ = re.compile(r'^kh: movie: #(\d+) (\S+) requested at game frame (\d+) \((.*)$')
END = re.compile(r'^kh: movie: (?:#(\d+)|\(dropped request\)) (\S+): the event (ended by itself|was ended by SKIP|was ended) at game frame (\d+)(.*)$')
TORN = re.compile(r'^kh: movie: #(\d+) (\S+): player torn down: reader released at game frame (\d+)')
GFX = re.compile(r'^kh: gfx frame=(\d+) .*? resources=(\d+)')


def parse(text):
    r = {'req': [], 'end': {}, 'torn': {}, 'torn_present': {}, 'dropped': 0, 'exit': None, 'fatal': [], 'res': {}, 'odd': []}
    present = None                                   # the number of the last `kh: gfx frame=` line read so far
    for l in text.splitlines():
        if not l.startswith('kh: '):
            continue
        m = GFX.match(l)
        if m:
            present = int(m.group(1)); r['res'][present] = int(m.group(2)); continue
        if l.startswith('kh: FATAL') or l.startswith('kh: SIGNAL'):
            r['fatal'].append(l[:160]); continue
        if l.startswith('kh: movies: '):
            r['exit'] = l; continue
        if not l.startswith('kh: movie: '):
            continue
        m = REQ.match(l)
        if m:
            r['req'].append({'n': int(m.group(1)), 'name': m.group(2), 'frame': int(m.group(3)), 'line': l, 'present': present}); continue
        m = END.match(l)
        if m:
            how = {'ended by itself': 'natural', 'was ended by SKIP': 'skip', 'was ended': 'other'}[m.group(3)]
            r['end'][int(m.group(1)) if m.group(1) else 0] = {'how': how, 'frame': int(m.group(4)), 'direct': 'runs DIRECTLY' in m.group(5)}; continue
        m = TORN.match(l)
        if m:
            r['torn'][int(m.group(1))] = int(m.group(3)); r['torn_present'][int(m.group(1))] = present; continue
        if 'REQUEST DROPPED BY THE GAME' in l:
            r['dropped'] += 1; continue
        r['odd'].append(l[:200])
    return r


def check(text, a):
    r = parse(text); bad = []; out = []
    def need(c, what):
        if not c: bad.append(what)
    if a.get('fatal'):
        need(len(r['fatal']) == 1 and r['fatal'][0].startswith('kh: FATAL: %s:' % a['fatal']),
             'the run must end with the one named fatal `%s`; it has: %s' % (a['fatal'], '; '.join(r['fatal'][:2]) or 'no fatal'))
        if len(r['fatal']) == 1: out.append('the expected named fatal: ' + r['fatal'][0][:150])
    else: need(not r['fatal'], 'the run has a fatal or a signal: %s' % '; '.join(r['fatal'][:2]))
    need(r['req'], 'no movie was requested')
    if a['first'] and r['req']:
        need(r['req'][0]['name'] == a['first'] + '.mp4', 'the first request is %s, not %s.mp4' % (r['req'][0]['name'], a['first']))
    if a.get('second'):
        need(len(r['req']) >= 2 and r['req'][1]['name'] == a['second'] + '.mp4', 'the second request is %s, not %s.mp4' % (r['req'][1]['name'] if len(r['req']) >= 2 else 'missing', a['second']))
    if a['movies'] is not None:
        need(len(r['req']) == a['movies'], '%d requests, expected %d' % (len(r['req']), a['movies']))
    if a['nopause'] and r['req']:
        says = 'this scene has NO pause screen' in r['req'][0]['line']
        need(says == (a['nopause'] == 'yes'), 'the first request %s that its scene has no pause screen' % ('does not say' if not says else 'says'))
    for q in r['req']:
        e = r['end'].get(q['n']); t = r['torn'].get(q['n'])
        out.append('#%d %s: requested at game frame %d; event: %s; player torn down: %s' % (q['n'], q['name'], q['frame'],
                   '%s at %d%s' % (e['how'], e['frame'], ' (DIRECT branch)' if e['direct'] else '') if e else 'NOT ENDED in this run', t if t is not None else 'no'))
        if a['mode'] == 'end':
            need(e is not None and e['how'] == 'natural', '#%d %s: its event did not end by itself (%s)' % (q['n'], q['name'], e['how'] if e else 'not ended'))
            need(e is None or not e['direct'], '#%d %s: its event ended through the direct branch (the player survives)' % (q['n'], q['name']))
            need(t is not None, '#%d %s: its player was not torn down' % (q['n'], q['name']))
            need(e is None or t is None or (q['frame'] <= e['frame'] <= t), '#%d %s: request, event end and teardown are out of order' % (q['n'], q['name']))
        elif a['mode'] == 'fail-skip':
            need('KHNMC_MOVIES=fail' in q['line'], '#%d %s: the request line does not say KHNMC_MOVIES=fail' % (q['n'], q['name']))
            need(e is not None and e['how'] == 'skip', '#%d %s: its event was not ended by Skip (%s)' % (q['n'], q['name'], e['how'] if e else 'not ended'))
    if a['mode'] == 'fail-wait' and r['req']:
        q = r['req'][0]
        need('KHNMC_MOVIES=fail' in q['line'], 'the first request line does not say KHNMC_MOVIES=fail')
        need(q['n'] not in r['end'], '#%d %s: its event ENDED (%s): the old seam must leave it waiting' % (q['n'], q['name'], r['end'].get(q['n'], {}).get('how')))
        if a['nopause'] == 'yes':
            need('FOR EVER' in q['line'] and "pause screen's Skip" not in q['line'], 'the request line of a scene without a pause screen must say FOR EVER and must not promise a Skip')
    if a['mode'] == 'end':
        need(r['dropped'] == 0, '%d requests were dropped by the game\'s guard' % r['dropped'])
        need(not r['odd'], 'unexpected `kh: movie:` lines: %s' % '; '.join(r['odd'][:2]))
        need(r['exit'] is not None, 'no exit line `kh: movies:` (the run did not end in order)')
        if r['exit']:
            need('mode end:' in r['exit'], 'the exit line is not the default mode\'s')
            need('NOT every' not in r['exit'], 'the exit line says NOT every requested movie ended by itself')
    if a['pictures'] and r['exit']:
        m = re.search(r'pictures: (\d+) made by the engine \((\d+) x (\d+), (\d+) bytes each, never freed by it\), (\d+) times the kept one handed out again, (\d+) kept ones lost, (\d+) not kept', r['exit'])
        need(m is not None, 'the exit line has no pictures part')
        if m:
            need((int(m.group(1)), int(m.group(5))) == a['pictures'], 'pictures: %s made, %s reused; expected %d, %d' % (m.group(1), m.group(5), a['pictures'][0], a['pictures'][1]))
            need(m.group(6) == '0' and m.group(7) == '0', 'pictures: %s kept ones lost, %s not kept' % (m.group(6), m.group(7)))
            out.append('pictures: %s made (%s x %s, %s bytes each), %s reused' % (m.group(1), m.group(2), m.group(3), m.group(4), m.group(5)))
    if a['steady']:
        need(len(r['req']) >= 2, '--steady-resources needs a run with at least two movies')
        for q in r['req'][1:]:
            pb = q['present']; pt = r['torn_present'].get(q['n'])
            b = r['res'].get(pb) if pb is not None else None; c = r['res'].get(pt + 5) if pt is not None else None
            need(b is not None and c is not None, '#%d %s: no `kh: gfx frame=` line before the request line or 5 presents after the teardown line' % (q['n'], q['name']))
            if b is not None and c is not None:
                need(pt >= pb, '#%d %s: the teardown line comes before the request line' % (q['n'], q['name']))
                need(b == c, '#%d %s: the movie made %d new D3D12 resources (resources=%d at present %d, the last before the request line; %d at present %d, 5 after the teardown line)' % (q['n'], q['name'], c - b, b, pb, c, pt + 5))
                out.append('#%d %s: resources=%d at present %d (the last before the request line), %d at present %d (5 after the teardown line)' % (q['n'], q['name'], b, pb, c, pt + 5))
    return bad, out


def selftest():
    n = [0]; f = [0]
    def T(c, what):
        n[0] += 1
        if not c: f[0] += 1; print('movie_run_check selftest FAIL: ' + what)
    A = lambda **k: dict({'first': None, 'second': None, 'movies': None, 'nopause': None, 'pictures': None, 'steady': False, 'mode': 'end', 'fatal': None}, **k)
    req = lambda i, nm, fr, extra='the pause screen is not disabled by the event\'s type (pause-disable bits 0x0)', tail='this port has no movie decoder': 'kh: movie: #%d %s.mp4 requested at game frame %d ("C:/x/%s.mp4"; the file is absent; %s): %s' % (i, nm, fr, nm, extra, tail)
    end = lambda i, nm, fr, how='ended by itself', br='its clean-up runs as a task (0x2cbe30)': 'kh: movie: #%d %s.mp4: the event %s at game frame %d, 4 game frames after the request (x); %s' % (i, nm, how, fr, br)
    torn = lambda i, nm, fr: 'kh: movie: #%d %s.mp4: player torn down: reader released at game frame %d after 1 video and 1 audio reads' % (i, nm, fr)
    ex = lambda made=1, reused=0, tail='': 'kh: movies: mode end: 1 requested (...); pictures: %d made by the engine (384 x 216, 331776 bytes each, never freed by it), %d times the kept one handed out again, 0 kept ones lost, 0 not kept%s' % (made, reused, tail)
    gfx = lambda fr, res: 'kh: gfx frame=%d bundles=1 kh: gfx: draws=1 presents=%d copies=1 psos=1 resources=%d upload/buffer bytes=1' % (fr, fr, res)
    good = '\n'.join([req(1, 'opn', 100), end(1, 'opn', 104), torn(1, 'opn', 107), ex()])
    T(check(good, A(first='opn', movies=1, nopause='no', pictures=(1, 0)))[0] == [], 'a good run passes')
    T(check(good, A(first='lop'))[0] != [], 'another first movie fails')
    T(check(good, A(movies=2))[0] != [], 'another number of movies fails')
    T(check(good, A(nopause='yes'))[0] != [], 'a wrong pause statement fails')
    T(check(good, A(pictures=(1, 1)))[0] != [], 'another picture count fails')
    T(check(good.replace(end(1, 'opn', 104), end(1, 'opn', 104, 'was ended by SKIP')), A())[0] != [], 'an event ended by Skip fails')
    T(check(good.replace(end(1, 'opn', 104), end(1, 'opn', 104, 'was ended')), A())[0] != [], 'an event ended another way fails')
    T(check(good.replace(end(1, 'opn', 104) + '\n', ''), A())[0] != [], 'an event that did not end fails (the reader line alone is not an end)')
    T(check(good.replace(torn(1, 'opn', 107) + '\n', ''), A())[0] != [], 'a player that was not torn down fails')
    T(check(good.replace(end(1, 'opn', 104), end(1, 'opn', 104, br='its clean-up runs DIRECTLY (0x2cbe30(0))')), A())[0] != [], 'the direct branch fails')
    T(check(good + '\nkh: movie: REQUEST DROPPED BY THE GAME: movie command 0 for x.mp4', A())[0] != [], 'a dropped request fails')
    T(check(good.replace(ex(), ex(tail='; NOT every requested movie ended by itself')), A())[0] != [], 'an exit line that says NOT every fails')
    T(check(good.replace(ex(), ''), A())[0] != [], 'a run without the exit line fails')
    T(check(good + '\nkh: FATAL: movie-open-failed: x', A())[0] != [], 'a fatal fails')
    T(check('kh: gfx frame=1 bundles=1 kh: gfx: draws=1 presents=1 copies=1 psos=1 resources=1 upload/buffer bytes=1', A())[0] != [], 'a run without a movie fails')
    nop = good.replace('the pause screen is not disabled by the event\'s type (pause-disable bits 0x0)', 'this scene has NO pause screen (pause-disable bits 0x2: the event\'s type has bit 7), so there is no Skip in it')
    T(check(nop, A(nopause='yes'))[0] == [] and check(nop, A(nopause='no'))[0] != [], 'the no-pause statement is read')
    # in log order; the presents run 11 ahead of the game frames (a title-idle run after one return to the title): the second request
    # (game frame 200) comes after present 211, its teardown (game frame 207) after present 218
    seq = lambda a, b, res: [gfx(f, res) for f in range(a, b + 1)]
    two = '\n'.join(seq(105, 106, 50) + [req(1, 'lop', 100)] + seq(107, 110, 52) + [end(1, 'lop', 104)] + seq(111, 113, 52) + [torn(1, 'lop', 107)] + seq(114, 120, 52)
                    + seq(195, 211, 70) + [req(2, 'lop', 200)] + seq(212, 215, 70) + [end(2, 'lop', 204)] + seq(216, 218, 70) + [torn(2, 'lop', 207)] + seq(219, 225, 70) + [ex(1, 1)])
    T(check(two, A(movies=2, pictures=(1, 1), steady=True))[0] == [], 'two movies, one picture, steady resources: passes')
    T(check(two.replace(gfx(223, 70), gfx(223, 72)), A(steady=True))[0] != [], 'a second movie that makes new resources fails')
    leak = two
    for pf in range(214, 226): leak = leak.replace(gfx(pf, 70), gfx(pf, 72))           # the picture of the second movie: made 3 presents after its request line
    T(check(leak, A(steady=True))[0] != [], 'a picture made after the request line fails (presents by log order, not by game frame number: presents 199 and 212 are both before the request)')
    T(check(two.replace(gfx(223, 70), ''), A(steady=True))[0] != [], 'steady resources without the counter lines fails')
    T(check(good, A(steady=True))[0] != [], 'steady resources with one movie fails')
    fw = req(1, 'ena', 100, 'this scene has NO pause screen (pause-disable bits 0x2: the event\'s type has bit 7), so there is no Skip in it', 'KHNMC_MOVIES=fail, the seam before task 44: the game queues no movie and its scene waits on a black screen FOR EVER: nothing ends it')
    T(check(fw, A(mode='fail-wait', nopause='yes', first='ena'))[0] == [], 'the old wait in a scene without a pause screen: passes')
    T(check(fw + '\n' + end(1, 'ena', 300, 'was ended by SKIP'), A(mode='fail-wait'))[0] != [], 'fail-wait with an ended event fails')
    T(check(fw.replace('FOR EVER: nothing ends it', "for the pause screen's Skip"), A(mode='fail-wait', nopause='yes'))[0] != [], 'fail-wait: a promised Skip in a no-pause scene fails')
    fs = req(1, 'opn', 100, tail='KHNMC_MOVIES=fail, the seam before task 44: x') + '\n' + end(1, 'opn', 176, 'was ended by SKIP')
    T(check(fs, A(mode='fail-skip'))[0] == [] and check(fs.replace('was ended by SKIP', 'ended by itself'), A(mode='fail-skip'))[0] != [], 'fail-skip wants Skip')
    T(check(two, A(first='lop', second='lop'))[0] == [] and check(two, A(second='me2'))[0] != [] and check(good, A(second='opn'))[0] != [], 'the second movie is read')
    ft = 'kh: FATAL: route-wait-timeout: x line 92 `@wait evt-program 0x38 timeout 3000`: never came'
    fo = req(1, 'opn', 100, tail='KHNMC_MOVIES=fail, the seam before task 44: x')
    T(check(fo + '\n' + ft, A(mode='fail-wait'))[0] != [], 'fail-wait: a fatal fails when none is expected')
    T(check(fo + '\n' + ft, A(mode='fail-wait', fatal='route-wait-timeout'))[0] == [], 'fail-wait with its expected named fatal: passes')
    T(check(fo, A(mode='fail-wait', fatal='route-wait-timeout'))[0] != [], 'an expected fatal that is not there fails')
    T(check(fo + '\n' + ft.replace('route-wait-timeout', 'movie-open-failed'), A(mode='fail-wait', fatal='route-wait-timeout'))[0] != [], 'another fatal than the expected one fails')
    T(check(fo + '\n' + ft + '\nkh: SIGNAL 11 (Segmentation fault)', A(mode='fail-wait', fatal='route-wait-timeout'))[0] != [], 'a signal beside the expected fatal fails')
    print('movie_run_check selftest: %d checks, %d failures' % (n[0], f[0])); return 1 if f[0] else 0


def main():
    v = sys.argv[1:]
    if v == ['--selftest']: return selftest()
    a = {'first': None, 'second': None, 'movies': None, 'nopause': None, 'pictures': None, 'steady': False, 'mode': 'end', 'fatal': None}; path = None; i = 0
    try:
        while i < len(v):
            if v[i] == '--first': a['first'] = v[i + 1]; i += 2
            elif v[i] == '--second': a['second'] = v[i + 1]; i += 2
            elif v[i] == '--expect-fatal': a['fatal'] = v[i + 1]; i += 2
            elif v[i] == '--movies': a['movies'] = int(v[i + 1]); i += 2
            elif v[i] == '--nopause' and v[i + 1] in ('yes', 'no'): a['nopause'] = v[i + 1]; i += 2
            elif v[i] == '--pictures': a['pictures'] = tuple(int(x) for x in v[i + 1].split(',')); i += 2; assert len(a['pictures']) == 2
            elif v[i] == '--steady-resources': a['steady'] = True; i += 1
            elif v[i] == '--mode' and v[i + 1] in ('end', 'fail-wait', 'fail-skip'): a['mode'] = v[i + 1]; i += 2
            elif not v[i].startswith('-') and path is None: path = v[i]; i += 1
            else: raise ValueError(v[i])
        if path is None: raise ValueError('no file')
    except (IndexError, ValueError, AssertionError):
        sys.stderr.write(__doc__ + '\n'); return 2
    bad, out = check(open(path, errors='replace').read(), a)
    for l in out: print('  ' + l)
    for b in bad: print('  NOT OK: ' + b)
    print('movie_run_check: %s: %s' % (path, 'OK' if not bad else '%d checks FAILED' % len(bad)))
    return 1 if bad else 0


sys.exit(main())
