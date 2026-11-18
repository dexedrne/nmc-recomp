#!/usr/bin/env python3
"""fill_compare.py : the pass conditions of the "fill" mode's first test (task 37, NATIVE-FILL-PLAN.md section 8 step 1).

  tools/fill_compare.py pairs REF_FRAMES TALL_FRAMES [--out REPORT.txt] [--png DIR] [thresholds]
      REF_FRAMES / TALL_FRAMES: the frames/ directories of the two runs (index.tsv + frame-NNNNNN.ppm, KHNMC_GFX_DUMP), REF at
      1920x1080 with nothing set, TALL at KHNMC_SCREEN=1920x1350 KHNMC_ASPECT=fill KHNMC_ASPECT_TEST_H1=unconditional; both on the
      frames clock (KHNMC_CLOCK=frames), the same route and difficulty.  Dumps are paired by game_frame (index.tsv); every dump that
      exists in both directories is measured.
  tools/fill_compare.py pick REF_FRAMES/index.tsv TALL_FRAMES/index.tsv
      prints the present numbers of the TALL run whose game_frame also has a REF dump (one line: "REF_PRESENTS ; TALL_PRESENTS"),
      for tools/darwin_route.sh frames; exit 1 when there is none.
  tools/fill_compare.py runcheck RUN.OUT tall WxH  |  runcheck RUN.OUT ref [WxH]
      the run's own report lines (exit 0 = PASS, 1 = FAIL).  tall: `kh: aspect report: KHNMC_ASPECT=fill + KHNMC_ASPECT_TEST_H1=
      unconditional`, H1 calls M > 0, rewritten to the full height N == M, result different from the model 0; `kh: screen:
      KHNMC_SCREEN=WxH; ... the built size checked at P presents ...: not WxH at 0` with P > 0; no `screen-not-applied` WARNING.
      ref: KHNMC_ASPECT=pc, calls > 0, narrow 0; with WxH the same screen line, without it no KHNMC_SCREEN in the run.
  tools/fill_compare.py selftest
      the checks below on synthetic pictures (a band shifted by 135 rows passes; black strips, a band off by one row, a misplaced
      element and a hard seam each fail the condition they should), and runcheck on synthetic report lines (14 cases: a pc run, a
      gated run, a run with no H1 call, a screen-not-applied run ... each fail).  Needs nothing but numpy.

The conditions, for every pair (REF picture A, W x 1080; TALL picture B, W x H with o = (H - 9W/16) / 2 = 135 at 1920x1350):
  (a) the same game moment and the expected sizes: both index rows have the same game_frame, A is W x 9W/16, B is W x H with an
      integer o.  The cumulative draw counts (index draws_total) are reported; a difference is printed as a NOTE (the null runs of
      task 37 show identical draw counts at both sizes on the frames clock, so a difference says the runs diverged).
  (b) THE BAND: rows o .. o+1079 of B against rows 0 .. 1079 of A, pixel for pixel.  The exe's projection is Vert+ below 16:9 and
      the band offset is a whole number of rows, so world geometry and the 2D layout land on the same pixels; what may still differ:
      (1) single edge pixels, from the float rounding of the scaled projection (m11 / r) and screen matrix (halfH * r);
      (2) soft areas behind the reduced-size post-effect targets, which the exe sizes from the screen: 960x675 / 480x337 against
      960x540 / 480x270 (null-backend census, task 37) - 1350 / 4 = 337.5 is truncated, so the quarter-size target is not an exact
      vertical scale of the band and glow / blur resamples at slightly different rows; (3) frame-sampling effects that still assume
      416 rows (pppEiDrawShipoly, plan H6; not in this step).  Tolerances (options): mean absolute difference over the band
      <= --band-mad 1.0 (0..255; task 21b measured 0.23 between two DIFFERENT renderers for one field frame and 0.11 between two
      consecutive presents of one renderer, so 1.0 leaves room for (2) and is still far below a shifted or rescaled picture, which
      is 10 and more); pixels whose largest channel difference is above 32 <= --band-gt32 0.2 % (one animation frame moved 0.14 %
      of the pixels above 32 in 21b: a misplaced HUD element or object exceeds it); and no tile of an 8 x 4 grid over the band with
      a mean difference above --tile-mad 3.0 (a local error - an element drawn at the wrong place, a culled object - concentrates
      in one tile while (1) and (2) spread out).  A band off by one row already fails the first two on any textured picture.
  (c) THE NEW ROWS: rows 0 .. o-1 (top strip) and o+1080 .. H-1 (bottom strip) of B.  (c1) no bars: each strip has at least
      --strip-nonblack 5 % pixels that are not pure black (bars are 0 %); (c2) no seam at the band edges: the mean step between the
      last strip row and the first band row (and between the last band row and the first bottom-strip row) is at most
      --edge-ratio 3.0 times the 95th percentile of the steps between neighbouring rows inside the band (a bar edge, a clear that
      covers only the band, or a culling edge across the width makes one hard horizontal line).  Also reported, not judged:
      mean colour, share of pure black, distinct colours and full-width black rows per strip (objects culled at the 208 half-height
      leave holes until plan step 5 / H2), and between consecutive pairs how much the strips change compared with the band (a strip
      that never changes while the band does may be stale: not cleared).
Exit status: 0 = every pair passes (a), (b) and (c); 1 = a pair fails or no pair could be measured; 2 = usage / unreadable input.
--png DIR (needs Pillow): per pair the two dumps as PNG and |band difference| x 8.
Reads binary PPM (P6, maxval 255) as written by KHNMC_GFX_DUMP.  Python 3 with numpy.
"""
import os
import re
import sys
import numpy as np


def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:2] != b'P6':
        raise SystemExit('fill_compare: %s: not a binary PPM (P6)' % path)
    pos, vals = 2, []
    while len(vals) < 3:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b'#':
            while data[pos:pos + 1] != b'\n':
                pos += 1
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        vals.append(int(data[pos:end])); pos = end
    pos += 1
    w, h, mx = vals
    if mx != 255:
        raise SystemExit('fill_compare: %s: maxval %d, expected 255' % (path, mx))
    a = np.frombuffer(data, dtype=np.uint8, count=w * h * 3, offset=pos)
    if a.size != w * h * 3:
        raise SystemExit('fill_compare: %s: short file' % path)
    return a.reshape(h, w, 3)


def read_index(path):
    """index.tsv of KHNMC_GFX_DUMP: present, file, width, height, ..., draws_total, game_frame, vblank"""
    rows = {}
    with open(path) as f:
        head = f.readline().rstrip('\n').split('\t')
        for need in ('present', 'file', 'width', 'height', 'draws_total', 'game_frame'):
            if need not in head:
                raise SystemExit('fill_compare: %s: no column %s (header %s)' % (path, need, head))
        for ln in f:
            c = dict(zip(head, ln.rstrip('\n').split('\t')))
            if not c.get('present'):
                continue
            r = {'present': int(c['present']), 'file': c['file'], 'w': int(c['width']), 'h': int(c['height']),
                 'draws': int(c['draws_total']), 'gf': int(c['game_frame'])}
            if r['gf'] in rows:
                print('fill_compare: NOTE: %s: game_frame %d has two dumps (presents %d and %d); the later one is used'
                      % (path, r['gf'], rows[r['gf']]['present'], r['present']))
            rows[r['gf']] = r
    return rows


DEFAULTS = {'band_mad': 1.0, 'band_gt32': 0.2, 'tile_mad': 3.0, 'edge_ratio': 3.0, 'strip_nonblack': 5.0}


def band_offset(w, h):
    ch = 9 * w / 16.0
    o = (h - ch) / 2.0
    return ch, o


def measure(a, b, th, label=''):
    """(a), (b), (c) for one pair; returns (ok, lines, strip_arrays, band_array)"""
    lines, ok = [], True
    ha, wa = a.shape[:2]; hb, wb = b.shape[:2]
    ch, o = band_offset(wb, hb)
    cond_a = wa == wb and ha * 16 == wa * 9 and o == int(o) and o > 0
    lines.append('  (a) sizes: REF %dx%d, TALL %dx%d, band offset o = %g rows: %s' % (wa, ha, wb, hb, o, 'PASS' if cond_a else 'FAIL'))
    if not cond_a:
        return False, lines, None, None
    o = int(o)
    band = b[o:o + ha].astype(np.int16)
    d = np.abs(band - a.astype(np.int16)); dm = d.max(axis=2); n = dm.size
    mad = float(d.mean()); gt32 = 100.0 * float((dm > 32).sum()) / n
    gh, gw = 4, 8
    tiles = [[float(d[gy * ha // gh:(gy + 1) * ha // gh, gx * wa // gw:(gx + 1) * wa // gw].mean()) for gx in range(gw)] for gy in range(gh)]
    worst = max(max(r) for r in tiles)
    ok_b = mad <= th['band_mad'] and gt32 <= th['band_gt32'] and worst <= th['tile_mad']
    lines.append('  (b) band: rows %d..%d of TALL against rows 0..%d of REF: mad %.4f (<= %g), pixels above 32 %.4f %% (<= %g %%), worst tile mad %.3f (<= %g), '
                 'identical pixels %.3f %%, above 8 %.4f %%, largest %d: %s'
                 % (o, o + ha - 1, ha - 1, mad, th['band_mad'], gt32, th['band_gt32'], worst, th['tile_mad'], 100.0 * float((dm == 0).sum()) / n,
                    100.0 * float((dm > 8).sum()) / n, int(dm.max()), 'PASS' if ok_b else 'FAIL'))
    lines.append('      mad per tile (8 x 4, top row first): ' + ' | '.join(' '.join('%.2f' % t for t in r) for r in tiles))
    ok = ok and ok_b
    # (c)
    top, bot = b[:o], b[o + ha:]
    bi = b.astype(np.int16)
    steps = np.abs(bi[o + 1:o + ha] - bi[o:o + ha - 1]).mean(axis=(1, 2))          # mean step between neighbouring band rows
    p95 = float(np.percentile(steps, 95))
    e_top = float(np.abs(bi[o] - bi[o - 1]).mean()); e_bot = float(np.abs(bi[o + ha] - bi[o + ha - 1]).mean())
    lim = th['edge_ratio'] * max(p95, 1.0)
    ok_c = True
    for name, s in (('top', top), ('bottom', bot)):
        key = (s[..., 0].astype(np.uint32) << 16) | (s[..., 1].astype(np.uint32) << 8) | s[..., 2]
        nonblack = 100.0 * float((key != 0).sum()) / key.size
        black_rows = int((key.max(axis=1) == 0).sum())
        c1 = nonblack >= th['strip_nonblack']
        ok_c = ok_c and c1
        lines.append('  (c1) %s strip (%d rows): not pure black %.3f %% (>= %g %%): %s; mean rgb (%.1f, %.1f, %.1f), distinct colours %d, full-width black rows %d'
                     % (name, s.shape[0], nonblack, th['strip_nonblack'], 'PASS' if c1 else 'FAIL (bars or nothing drawn)',
                        s[..., 0].mean(), s[..., 1].mean(), s[..., 2].mean(), int(np.unique(key).size), black_rows))
    c2 = e_top <= lim and e_bot <= lim
    ok_c = ok_c and c2
    lines.append('  (c2) band edges: mean step across the top edge (rows %d|%d) %.3f, bottom edge (rows %d|%d) %.3f; inside the band: median %.3f, 95th percentile %.3f; limit %g x max(p95, 1) = %.3f: %s'
                 % (o - 1, o, e_top, o + ha - 1, o + ha, e_bot, float(np.median(steps)), p95, th['edge_ratio'], lim, 'PASS' if c2 else 'FAIL (a hard horizontal line at the band edge)'))
    ok = ok and ok_c
    return ok, lines, (top, bot), b[o:o + ha]


def save_png(path, arr):
    from PIL import Image
    Image.fromarray(arr, 'RGB').save(path)


def pairs(ref_dir, tall_dir, th, out=None, png=None):
    ri = read_index(os.path.join(ref_dir, 'index.tsv')); ti = read_index(os.path.join(tall_dir, 'index.tsv'))
    common = sorted(set(ri) & set(ti))
    rep = ['fill_compare: REF %s (%d dumps in its index), TALL %s (%d); game frames in both indexes: %d' % (ref_dir, len(ri), tall_dir, len(ti), len(common))]
    measured, failed, prev = 0, 0, None
    for gf in common:
        r, t = ri[gf], ti[gf]
        pa, pb = os.path.join(ref_dir, r['file']), os.path.join(tall_dir, t['file'])
        if not (os.path.exists(pa) and os.path.exists(pb)):
            rep.append('game frame %d: REF present %d %s, TALL present %d %s: not both here (fetch them: tools/darwin_route.sh frames TAG PRESENT)'
                       % (gf, r['present'], 'present' if os.path.exists(pa) else 'MISSING', t['present'], 'present' if os.path.exists(pb) else 'MISSING'))
            continue
        a, b = read_ppm(pa), read_ppm(pb)
        rep.append('game frame %d: REF present %d (%dx%d, draws_total %d), TALL present %d (%dx%d, draws_total %d)%s'
                   % (gf, r['present'], a.shape[1], a.shape[0], r['draws'], t['present'], b.shape[1], b.shape[0], t['draws'],
                      '' if (r['draws'] == t['draws'] and r['present'] == t['present']) else '   NOTE: presents or draw counts differ: the runs may have diverged'))
        ok, lines, strips, band = measure(a, b, th)
        rep += lines
        if strips is not None and prev is not None:
            pt, pbnd = prev
            def chg(x, y): return float(np.abs(x.astype(np.int16) - y.astype(np.int16)).mean())
            rep.append('  (info) change since the previous pair: band %.3f, top strip %.3f, bottom strip %.3f%s'
                       % (chg(band, pbnd), chg(strips[0], pt[0]), chg(strips[1], pt[1]),
                          '   NOTE: the strips did not change while the band did (stale content?)' if chg(band, pbnd) > 0.5 and chg(strips[0], pt[0]) == 0 and chg(strips[1], pt[1]) == 0 else ''))
        if strips is not None:
            prev = (strips, band)
        if png:
            os.makedirs(png, exist_ok=True)
            save_png(os.path.join(png, 'gf%06d-ref.png' % gf), a); save_png(os.path.join(png, 'gf%06d-tall.png' % gf), b)
            if band is not None:
                d = np.abs(band.astype(np.int16) - a.astype(np.int16))
                save_png(os.path.join(png, 'gf%06d-band-diff-x8.png' % gf), np.clip(d.astype(np.int32) * 8, 0, 255).astype(np.uint8))
        rep.append('  => %s' % ('PASS' if ok else 'FAIL'))
        measured += 1; failed += 0 if ok else 1
    rep.append('fill_compare: %d pairs measured, %d passed, %d failed%s' % (measured, measured - failed, failed, '' if measured else ' (NOTHING MEASURED: no pair of dumps with the same game frame is here)'))
    txt = '\n'.join(rep)
    print(txt)
    if out:
        with open(out, 'w') as f:
            f.write(txt + '\n')
    return 0 if measured and not failed else 1


H1_RE = re.compile(r'^kh: aspect report: KHNMC_ASPECT=(\w+)( \+ KHNMC_ASPECT_TEST_H1=unconditional)?; H1 \(0x11c1e0\): calls (\d+), not narrow (\d+), '
                   r'narrow (\d+) \(rewritten to the full height (\d+), kept: [^)]*? (\d+)\), result different from the model (\d+);.*? map pass \(0x186920\) calls (\d+);')
SCREEN_RE = re.compile(r'^kh: screen: KHNMC_SCREEN=(\d+)x(\d+); window-size requests not applied .*?the built size checked at (\d+) presents [^:]*: not \d+x\d+ at (\d+)$')


def runcheck(run_out, role, screen):
    """the run's own report lines (task 37 review): what the TALL run must show beyond its pictures, and the REF run's sanity.
    tall: the H1 report says KHNMC_ASPECT=fill + KHNMC_ASPECT_TEST_H1=unconditional, calls M > 0, rewritten to the full height
          N == M (unconditional H1 on a narrow screen rewrites every call; a call left as the exe made it is a failure here), result
          different from the model 0; the screen report says KHNMC_SCREEN=SCREEN, the built size checked at P > 0 presents and not
          SCREEN at 0; no `screen-not-applied` WARNING anywhere.
    ref:  the H1 report says KHNMC_ASPECT=pc, calls M > 0, narrow 0 (a 16:9 screen); with --screen, the screen line as above; without
          it, no KHNMC_SCREEN at all.
    The last report line of each kind is used.  Returns (ok, lines)."""
    try:
        with open(run_out, 'rb') as f:
            text = f.read().decode('utf-8', 'replace').splitlines()
    except OSError as e:
        return False, ['  run.out %s: cannot be read (%s): FAIL' % (run_out, e)]
    lines, ok = [], True
    def say(good, what):
        nonlocal ok
        ok = ok and good
        lines.append('  %s: %s' % (what, 'PASS' if good else 'FAIL'))
    h1 = [m for m in (H1_RE.match(l) for l in text) if m]
    if not h1:
        say(False, '%s: no `kh: aspect report: KHNMC_ASPECT=... H1 (0x11c1e0): calls ...` line (the build lacks task 37, or KHNMC_ASPECT / KHNMC_ASPECT_LOG did not reach it)' % role)
    else:
        m = h1[-1]
        mode, uncond = m.group(1), bool(m.group(2))
        calls, wide, narrow, rew, kept, mism, mp = (int(m.group(i)) for i in (3, 4, 5, 6, 7, 8, 9))
        desc = 'KHNMC_ASPECT=%s%s, H1 calls %d, not narrow %d, narrow %d, rewritten %d, kept %d, different from the model %d, map pass %d' % (
            mode, ' + unconditional' if uncond else '', calls, wide, narrow, rew, kept, mism, mp)
        if role == 'tall':
            say(mode == 'fill' and uncond, '%s: H1 mode (%s): fill + KHNMC_ASPECT_TEST_H1=unconditional required' % (role, desc))
            say(calls > 0 and rew == calls and mism == 0, '%s: H1 active: rewritten to the full height %d of %d calls, %d different from the model (every call rewritten, at least one, none different)' % (role, rew, calls, mism))
        else:
            say(mode == 'pc' and calls > 0 and narrow == 0, '%s: H1 (%s): pc, at least one call, none narrow (16:9) required' % (role, desc))
    scr = [m for m in (SCREEN_RE.match(l) for l in text) if m]
    if screen:
        sw, sh = screen
        if not scr:
            say(False, '%s: no `kh: screen: KHNMC_SCREEN=%dx%d; ... the built size checked at P presents ...` exit-report line' % (role, sw, sh))
        else:
            m = scr[-1]
            w, h, np_, nb = (int(m.group(i)) for i in (1, 2, 3, 4))
            say((w, h) == (sw, sh) and np_ > 0 and nb == 0, '%s: screen KHNMC_SCREEN=%dx%d (expected %dx%d), the built size checked at %d presents, not the screen at %d (at least one checked, none wrong)' % (role, w, h, sw, sh, np_, nb))
    else:
        say(not scr and not any(l.startswith('kh: screen: KHNMC_SCREEN=') for l in text), '%s: no KHNMC_SCREEN in the run (REF at 1920x1080 with nothing set)' % role)
    warn = [l for l in text if 'WARNING: screen-not-applied' in l]
    say(not warn, '%s: `screen-not-applied` WARNING lines: %d%s' % (role, len(warn), (' (first: %s)' % warn[0][:200]) if warn else ''))
    return ok, lines


def pick(ref_index, tall_index):
    ri, ti = read_index(ref_index), read_index(tall_index)
    common = sorted(set(ri) & set(ti))
    if not common:
        print('fill_compare: no game frame has a dump in both indexes', file=sys.stderr); return 1
    print(' '.join(str(ri[g]['present']) for g in common) + ' ; ' + ' '.join(str(ti[g]['present']) for g in common))
    return 0


def selftest():
    """synthetic pictures: a textured 'world' seen through a 16:9 window and through a taller window centred on the same point"""
    rng = np.random.default_rng(37)
    W, H, HB = 384, 216, 270                          # a fifth of 1920x1080 / 1920x1350: o = (270 - 216) / 2 = 27 rows
    world = np.zeros((HB + 40, W, 3), np.uint8)
    yy, xx = np.mgrid[0:world.shape[0], 0:W]
    world[..., 0] = (64 + 60 * np.sin(xx / 13.0) + 40 * np.cos(yy / 7.0)).clip(0, 255)
    world[..., 1] = (90 + 50 * np.cos((xx + yy) / 11.0)).clip(0, 255)
    world[..., 2] = (120 + 30 * np.sin(yy / 5.0) + rng.integers(0, 6, size=(world.shape[0], W))).clip(0, 255)
    top = 20                                          # the tall picture's first row in the world
    o = (HB - H) // 2
    tall = world[top:top + HB].copy()
    ref = world[top + o:top + o + H].copy()
    th = dict(DEFAULTS)
    n, fails = 0, 0
    def expect(name, a, b, want_ok, want_tag=None):
        nonlocal n, fails
        ok, lines, _, _ = measure(a, b, th)
        txt = '\n'.join(lines)
        tagged = [l for l in lines if l.startswith('  ' + want_tag)] if want_tag else []
        good = ok == want_ok and (want_tag is None or (tagged and any(': FAIL' in l for l in tagged)))   # the named condition itself failed
        n += 1; fails += 0 if good else 1
        print('fill_compare selftest: %-44s %s (expected %s%s)' % (name, 'PASS' if ok else 'FAIL', 'PASS' if want_ok else 'FAIL', ', ' + want_tag + ' failing' if want_tag else '') + ('' if good else '   <-- WRONG\n' + txt))
    expect('band shifted by o, new rows filled', ref, tall, True)
    noisy = tall.copy(); m = rng.random(noisy.shape[:2]) < 0.001; noisy[m] = 255 - noisy[m]   # 0.1 %: single edge pixels
    expect('the same with 0.1 % single-pixel flips', ref, noisy, True)
    noisy2 = tall.copy(); m = rng.random(noisy2.shape[:2]) < 0.01; noisy2[m] = 255 - noisy2[m]  # 1 %: too many
    expect('1 % pixel flips in the band', ref, noisy2, False, '(b)')
    bars = tall.copy(); bars[:o] = 0; bars[o + H:] = 0
    expect('black bars (no H1)', ref, bars, False, '(c1) top strip')
    off1 = world[top + 1:top + 1 + HB].copy()
    expect('band off by one row', ref, off1, False, '(b)')
    elem = tall.copy(); elem[o + 20:o + 50, 40:90] = elem[o + 120:o + 150, 40:90]             # an element drawn at the wrong place
    expect('a misplaced element in the band', ref, elem, False, '(b)')
    seam = tall.copy(); seam[:o] = np.clip(seam[:o].astype(np.int16) + 90, 0, 255).astype(np.uint8)   # strip content that does not continue the band
    expect('a hard seam at the top band edge', ref, seam, False, '(c2)')
    wrong = np.zeros((H + 1, W, 3), np.uint8)
    expect('wrong reference size', wrong, tall, False, '(a)')
    # runcheck: the report lines of the two runs, in the formats runtime/aspect.c and runtime/null_window.c print
    import tempfile
    def h1(mode, unc, calls, narrow, rew, kept, mism=0, mp=500):
        return ('kh: aspect report: KHNMC_ASPECT=%s%s; H1 (0x11c1e0): calls %d, not narrow %d, narrow %d (rewritten to the full height %d, kept: no 3D view in this frame or the previous one %d), '
                'result different from the model %d; narrow calls by game frames since the map pass: same frame 500, previous frame 0, 2 or more frames earlier 55, never 845; map pass (0x186920) calls %d; first guest threads: H1 1012, map pass 1012'
                % (mode, ' + KHNMC_ASPECT_TEST_H1=unconditional' if unc else '', calls, calls - narrow, narrow, rew, kept, mism, mp))
    def scr(w, h, np_, nb):
        return ['kh: screen: KHNMC_SCREEN=%dx%d: the monitor, the one display mode, the client rectangle and the DXGI output are %dx%d; the window is maximized (IsZoomed=1) from the first call; what the game builds is checked at every present (WARNING screen-not-applied when it is another size)' % (w, h, w, h),
                'kh: screen: KHNMC_SCREEN=%dx%d; window-size requests not applied (the window stays maximized at the screen size): CreateWindowExA 0, SetWindowPos 0; the built size checked at %d presents (swap chain, GX+0x8, GX+0x18): not %dx%d at %d' % (w, h, np_, w, h, nb)]
    good_tall = scr(1920, 1350, 1406, 0) + [h1('fill', True, 1400, 1400, 1400, 0)]
    rc_cases = [
        ('tall: fill + unconditional, every call rewritten', 'tall', (1920, 1350), good_tall, True),
        ('tall: H1 in pc (the old grep accepted it)', 'tall', (1920, 1350), scr(1920, 1350, 1406, 0) + [h1('pc', False, 1400, 1400, 0, 0)], False),
        ('tall: gated fill (6108 of 7000 rewritten)', 'tall', (1920, 1350), scr(1920, 1350, 7006, 0) + [h1('fill', False, 7000, 7000, 6108, 892)], False),
        ('tall: unconditional but no call at all', 'tall', (1920, 1350), scr(1920, 1350, 1406, 0) + [h1('fill', True, 0, 0, 0, 0)], False),
        ('tall: one call left as the exe made it', 'tall', (1920, 1350), scr(1920, 1350, 1406, 0) + [h1('fill', True, 1400, 1400, 1399, 0, 1)], False),
        ('tall: no aspect report line', 'tall', (1920, 1350), scr(1920, 1350, 1406, 0), False),
        ('tall: built size not the screen at 3 presents', 'tall', (1920, 1350), scr(1920, 1350, 1406, 3) + [h1('fill', True, 1400, 1400, 1400, 0)], False),
        ('tall: a screen-not-applied WARNING', 'tall', (1920, 1350), good_tall + ['kh: screen: WARNING: screen-not-applied: KHNMC_SCREEN=1920x1350, but at present 5 ...'], False),
        ('tall: no screen report line', 'tall', (1920, 1350), [h1('fill', True, 1400, 1400, 1400, 0)], False),
        ('tall: another screen size', 'tall', (1920, 1350), scr(1920, 1080, 1406, 0) + [h1('fill', True, 1400, 1400, 1400, 0)], False),
        ('ref: pc at 16:9, nothing set', 'ref', None, [h1('pc', False, 1400, 0, 0, 0)], True),
        ('ref: KHNMC_SCREEN set where none is expected', 'ref', None, scr(1920, 1350, 1406, 0) + [h1('pc', False, 1400, 0, 0, 0)], False),
        ('ref: narrow calls (not 16:9)', 'ref', None, [h1('pc', False, 1400, 1400, 0, 0)], False),
        ('ref: KHNMC_SCREEN=2560x1440 as asked', 'ref', (2560, 1440), scr(2560, 1440, 1406, 0) + [h1('pc', False, 1400, 0, 0, 0)], True),
    ]
    with tempfile.TemporaryDirectory() as td:
        for k, (name, role, sc, lines_, want) in enumerate(rc_cases):
            p = os.path.join(td, 'run%02d.out' % k)
            with open(p, 'w') as f:
                f.write('kh: gfx frame=1 bundles=0\n' + '\n'.join(lines_) + '\n')
            ok, out = runcheck(p, role, sc)
            good = ok == want
            n += 1; fails += 0 if good else 1
            print('fill_compare selftest: runcheck %-50s %s (expected %s)' % (name, 'PASS' if ok else 'FAIL', 'PASS' if want else 'FAIL') + ('' if good else '   <-- WRONG\n' + '\n'.join(out)))
    print('fill_compare selftest: %d cases, %d wrong' % (n, fails))
    return 1 if fails else 0


def main():
    av = sys.argv[1:]
    if not av or av[0] in ('-h', '--help'):
        print(__doc__); return 0 if av else 2
    th = dict(DEFAULTS)
    def opt(name, conv=str, default=None):
        if name in av:
            i = av.index(name)
            if i + 1 >= len(av):
                raise SystemExit('fill_compare: %s needs a value' % name)
            v = conv(av[i + 1]); del av[i:i + 2]; return v
        return default
    for k in DEFAULTS:
        v = opt('--' + k.replace('_', '-'), float)
        if v is not None:
            th[k] = v
    out = opt('--out'); png = opt('--png')
    if av[0] == 'pairs' and len(av) == 3:
        for d in av[1:]:
            if not os.path.isfile(os.path.join(d, 'index.tsv')):
                print('fill_compare: %s/index.tsv is missing' % d, file=sys.stderr); return 2
        return pairs(av[1], av[2], th, out, png)
    if av[0] == 'pick' and len(av) == 3:
        return pick(av[1], av[2])
    if av[0] == 'runcheck' and len(av) in (3, 4) and av[2] in ('tall', 'ref'):
        sc = None
        if len(av) == 4:
            m = re.fullmatch(r'(\d+)x(\d+)', av[3])
            if not m:
                print('fill_compare: runcheck: the screen must be WxH, not %s' % av[3], file=sys.stderr); return 2
            sc = (int(m.group(1)), int(m.group(2)))
        if av[2] == 'tall' and not sc:
            print('fill_compare: runcheck RUN.OUT tall needs the screen (WxH)', file=sys.stderr); return 2
        ok, out = runcheck(av[1], av[2], sc)
        print('fill_compare: runcheck %s (%s%s): %s' % (av[1], av[2], ', KHNMC_SCREEN=%dx%d' % sc if sc else '', 'PASS' if ok else 'FAIL'))
        print('\n'.join(out))
        return 0 if ok else 1
    if av[0] == 'selftest' and len(av) == 1:
        return selftest()
    print(__doc__); return 2


if __name__ == '__main__':
    sys.exit(main())
