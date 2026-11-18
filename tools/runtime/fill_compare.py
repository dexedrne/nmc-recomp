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
  task 40 (H2 / H4 / H5; the GPU runs are not frame-identical past the field, so these compare by CONTENT, never by present number):
  tools/fill_compare.py tallcheck BAND.TSV
      one TALL run's KHNMC_GFX_BANDLOG (runtime/gfx_vkd3d.c: per present the 16:9 band and the strips above / below it).  (f) every
      present whose band is pure black (a full-screen fade or a black screen) has pure black strips too (non-black share <= --fade-strip
      0.01 %%): a fade that covers only the band leaves the scene in the strips (plan H4); (s) every present whose band and strips are
      not pure black (a pure black strip with a lit band = the exe's own bars on a frame without the 3D view, H1's gate: counted, not
      judged) has no hard line at or just beyond the band edges: max(edge_top_max, edge_bottom_max) <= --edge-ratio 3 x
      max(band_step_p95, 1).  edge_*_max are the largest row steps over the W = ceil(2 ch / 416) row pairs at and beyond each band edge
      (task 40 review: the exe's quads overshoot the band by about 3 rows at 1920x1350, so a fade over the band only draws its line
      there, not on the band-edge row pair).  A band log without those columns is the named error band-log-too-old (exit 2).  Exit 0 =
      PASS (both hold at every present and at least one present is narrower than 16:9), 1 = FAIL.  With H4 off
      (KHNMC_ASPECT_TEST_OFF=h4) the same window must FAIL on BOTH (f) and (s): the control that the window holds a full fade and a
      partial one.
  tools/fill_compare.py bandpairs A_BAND.TSV B_BAND.TSV [--min-pairs N]
      pairs every present of A with a present of B whose band hash is the same (the same game state: camera, objects, HUD) whatever
      their present numbers, and compares the strips' hashes.  Only INFORMATIVE pairs count: pure black bands are skipped, and so are
      pairs whose strips are pure black in both runs (the exe's bars: title, menus).  Every pair must have equal strips, at least N
      pairs (default 20).  Exit 0 = PASS.  On its own this says nothing about H2 (task 40 review): see h2pictures.
  tools/fill_compare.py h2pictures TALL_BAND.TSV NOCULL_BAND.TSV OFFH2_BAND.TSV [--min-pairs N]
      the H2 picture test with its control.  TALL (H2 on) against the no-cull reference (KHNMC_ASPECT_TEST_NOCULL=1): every
      informative pair equal, at least N; the control OFFH2 (KHNMC_ASPECT_TEST_OFF=h2) against TALL: the band states where their strips
      differ are moments where H2 shows content the 16:9 cull removes.  Exit 0 = PASS (at least one such moment, and TALL = no-cull at
      every pair), 1 = FAIL (too few pairs; or TALL differs from the no-cull reference where H2 is not seen at work), 3 = NOT
      DEMONSTRATED (the control never differs from TALL, or shares too few moments with it: TALL = no-cull then says nothing about H2),
      4 = NOT EXACT (H2 visibly at work, and TALL differs from the no-cull reference only within 2 presents of those moments: an effect
      whose state depends on when it first passed the cull, measured on the story route, cannot be told from a loss here; judge the
      dumps).
  tools/fill_compare.py hashprefix REF_HASH.TSV NEW_HASH.TSV [--min-presents N]
      two KHNMC_GFX_HASHLOG files of one screen size (16:9: nothing set against KHNMC_ASPECT=fill), compared by present number only up
      to the first present whose cumulative draw count differs (GPU runs may become different moments after that; task 40 review).
      Exit 0 = PASS (every picture there equal, through present N or later; default 970 = the census route's title, menus and
      map-transition fade), 1 = FAIL (a picture differs while the draw counts are equal), 3 = UNDECIDED (the draw counts part before N).
  tools/fill_compare.py hashcontent REF_HASH.TSV NEW_HASH.TSV [--min-share S]
      (information only, no longer a pass condition: it fails correct runs that diverge) the share of NEW pictures REF also showed.
  tools/fill_compare.py runcheck RUN.OUT fill WxH | fill169 [WxH] | nocull WxH | off WxH LIST
      fill: the mode as played: KHNMC_ASPECT=fill, no TEST AID line, H1 calls > 0 and rewritten > 0, none different from the model;
      the screen line as for tall; H2b and H2c: every call divided by r and restored, none off, changed inside 0, camera not guest
      memory 0; H4 rewritten > 0, not off.
      fill169: KHNMC_ASPECT=fill on a 16:9 screen: no narrow H1 call, every H2b / H2c call "r = 1, nothing written", H4 / H5 wrote 0.
      nocull: as fill, but the TEST AID line names the no-cull reference and every H2 call divided by 2047/208.
      off: as fill, but the TEST AID line names exactly the hooks of LIST (KHNMC_ASPECT_TEST_OFF's value) and each of them passed every
      call through and wrote nothing (a test-aid run whose aid did not take effect would equal TALL and pass the picture checks).
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


def read_bandlog(path):
    """KHNMC_GFX_BANDLOG (task 40): list of dicts per present; strip columns '-' (16:9 or wider) become None"""
    rows = []
    with open(path) as f:
        head = f.readline().rstrip('\n').split('\t')
        need = ('present', 'band_hash', 'top_hash', 'bottom_hash', 'band_nonblack_pct', 'top_nonblack_pct', 'bottom_nonblack_pct', 'edge_top', 'edge_bottom', 'band_step_p95')
        for k in need:
            if k not in head:
                raise SystemExit('fill_compare: %s: not a KHNMC_GFX_BANDLOG file (no column %s)' % (path, k))
        for ln in f:
            c = dict(zip(head, ln.rstrip('\n').split('\t')))
            if not c.get('present'):
                continue
            num = lambda k: None if c.get(k, '-') == '-' else float(c[k])
            rows.append({'present': int(c['present']), 'gf': int(c.get('game_frame', '0')), 'band': c['band_hash'], 'top': None if c['top_hash'] == '-' else c['top_hash'],
                         'bottom': None if c['bottom_hash'] == '-' else c['bottom_hash'], 'bn': num('band_nonblack_pct'), 'tn': num('top_nonblack_pct'),
                         'on': num('bottom_nonblack_pct'), 'et': num('edge_top'), 'eb': num('edge_bottom'), 'p95': num('band_step_p95'),
                         'haswin': 'edge_top_max' in head, 'win': num('edge_window_rows'), 'etw': num('edge_top_max'), 'ebw': num('edge_bottom_max')})
    return rows


class BandlogTooOld(Exception):
    pass


def tallcheck(rows, fade_strip=0.01, edge_ratio=3.0):
    """(f) pure black band -> pure black strips; (s) no hard line at or just beyond the band edges.  -> (ok, lines)
    (s) uses the edge WINDOWS (task 40 review finding 3: the exe's quads overshoot the band by about 3 rows at 1920x1350, so a fade
    over the band only makes its line outside the one row pair edge_top / edge_bottom measure): max(edge_top_max, edge_bottom_max)
    must stay <= edge_ratio x max(band_step_p95, 1).  A band log without those columns raises BandlogTooOld (a named error, exit 2)."""
    narrow = [r for r in rows if r['top'] is not None]
    if narrow and not narrow[0]['haswin']:
        raise BandlogTooOld('fill_compare: tallcheck: band-log-too-old: no columns edge_window_rows / edge_top_max / edge_bottom_max (a band log written '
                            'before the task-40 review): (s) cannot be judged; rerun with a build that writes them')
    black = [r for r in narrow if r['bn'] == 0]
    fbad = [r for r in black if r['tn'] > fade_strip or r['on'] > fade_strip]
    lit = [r for r in narrow if r['bn'] > 0 and r['tn'] > 0 and r['on'] > 0]
    bars = [r for r in narrow if r['bn'] > 0 and (r['tn'] == 0 or r['on'] == 0)]       # the exe's own bars (frames without the 3D view: H1 is gated)
    sbad = [r for r in lit if max(r['etw'], r['ebw']) > edge_ratio * max(r['p95'], 1.0)]
    old = [r for r in lit if max(r['et'], r['eb']) > edge_ratio * max(r['p95'], 1.0)]
    lines = ['  presents %d, narrower than 16:9 %d' % (len(rows), len(narrow)),
             '  (f) presents with a pure black band %d; of them with strips not pure black (> %g %%) %d%s: %s'
             % (len(black), fade_strip, len(fbad), (' (first: ' + ' '.join('%d/gf%d top %.2f%% bottom %.2f%%' % (r['present'], r['gf'], r['tn'], r['on']) for r in fbad[:6]) + ')') if fbad else '',
                'PASS' if not fbad else 'FAIL (a full-screen fade or black screen covers only the 16:9 band)'),
             '  (s) presents with a lit band and lit strips %d (with a lit band and a pure black strip, i.e. the exe\'s bars of a frame without the 3D view: %d, not judged); of them with a hard line within %s rows at or beyond a band edge (> %g x max(p95, 1)) %d%s (on the band-edge row pair alone: %d): %s'
             % (len(lit), len(bars), '/'.join(sorted(set('%d' % r['win'] for r in lit))) or '-', edge_ratio, len(sbad),
                (' (first: ' + ' '.join('%d/gf%d window max %.1f/%.1f edge rows %.1f/%.1f p95 %.1f' % (r['present'], r['gf'], r['etw'], r['ebw'], r['et'], r['eb'], r['p95']) for r in sbad[:6]) + ')') if sbad else '',
                len(old), 'PASS' if not sbad else 'FAIL (a seam at the band edge: a partial fade or bars over the band only)')]
    return bool(narrow) and not fbad and not sbad, lines


def informative_pairs(ra, rb):
    """pairs of A presents with a B present of the same 16:9 band hash (the same game state), skipping pure black bands and the pairs
    whose strips are pure black in both runs (the exe's bars on frames without the 3D view: nothing there can be culled or not).
    -> list of (a, b)"""
    bk = {}
    for r in rb:
        if r['bn'] and r['bn'] > 0:
            bk.setdefault(r['band'], r)
    out = []
    for r in ra:
        if not (r['bn'] and r['bn'] > 0) or r['band'] not in bk:
            continue
        b = bk[r['band']]
        if r['top'] is not None and (r['tn'] or 0) == 0 and (r['on'] or 0) == 0 and (b['tn'] or 0) == 0 and (b['on'] or 0) == 0:
            continue
        out.append((r, b))
    return out


def bandpairs(ra, rb, min_pairs=20):
    """informative pairs by band hash; every pair must have equal strips.  -> (ok, lines)"""
    pairs = informative_pairs(ra, rb)
    bad = [(a, b) for a, b in pairs if a['top'] != b['top'] or a['bottom'] != b['bottom']]
    lines = ['  A %d presents, B %d; pairs with the same (not pure black) 16:9 band and strips not pure black in both: %d (A presents %s); with different strips: %d%s'
             % (len(ra), len(rb), len(pairs), ('%d..%d' % (pairs[0][0]['present'], pairs[-1][0]['present'])) if pairs else '-', len(bad),
                (' (first: ' + ' '.join('A%d=B%d' % (a['present'], b['present']) for a, b in bad[:8]) + ')') if bad else ''),
             '  => %s' % ('PASS' if pairs and len(pairs) >= min_pairs and not bad else
                          'FAIL (%s)' % ('strips differ in %d pairs' % len(bad) if bad else 'only %d pairs (at least %d needed): the runs never showed the same moment' % (len(pairs), min_pairs)))]
    return bool(pairs) and len(pairs) >= min_pairs and not bad, lines


H2_NEAR = 2          # h2pictures: a TALL / no-cull difference within this many TALL presents of a moment where H2 visibly works


def h2pictures(rt, rn, ro, min_pairs=20):
    """the H2 picture test with its control (task 40 review finding 1).  rt = the TALL run (H2 on), rn = the no-cull reference
    (KHNMC_ASPECT_TEST_NOCULL=1), ro = the control (KHNMC_ASPECT_TEST_OFF=h2: the 16:9 cull).  All pairs by band hash (informative pairs
    only, any present numbers).  Two facts:
      C = the moments at which the control's strips differ from TALL's (H2 visibly changes the strips: it shows content the 16:9 cull
          removes); E = the moments at which TALL's strips differ from the no-cull reference's.
    -> (code, lines):
      0 PASS: C not empty and E empty (H2 at work, and the strips exactly the reference's).
      3 NOT DEMONSTRATED: C and E empty (nothing in this window that the 16:9 cull removes from the strips: TALL = reference says
        nothing about H2), or E not empty while the control shared fewer than min_pairs moments with TALL (H2 not shown at work).
      4 NOT EXACT: C not empty, E not empty, and every E moment lies within H2_NEAR (2) TALL presents of a C moment: TALL differs from
        the reference only where H2 visibly works.  Measured in the task-40 review (story route, TALL presents 1111-1163, lavapipe): an
        effect that H2c brings into the top strip is drawn in another state by the no-cull reference: brighter than TALL at presents
        1124-1145 and fainter at 1147-1160 (top-strip means): its animation runs from another start.  The null backend's draw log
        (KHNMC_ASPECT_M_DRAWLOG, run-Z40/rev/drawlog-fix-sh2-*.tsv) shows why: its 4 draws (3D VS, 430 indices, call chain 0x4b3c85 <
        0x4b71db) are made by the reference from the field arrival (present 900) on, while it is wholly off the screen, by TALL from
        present 1103 on (when it reaches the tall frustum), and with the 16:9 cull (H2 or H2c off) not before 1175; from 1103 TALL and
        the reference make the same draws.  The band log cannot tell such a state shift from content H2 leaves out: the dumps decide.
      1 FAIL: fewer than min_pairs TALL / reference moments; or E not empty and C empty with at least min_pairs control moments (H2
        changes nothing visible, yet the reference shows more); or an E moment farther than H2_NEAR presents from every C moment (TALL
        differs from the reference where H2 is not seen at work)."""
    tp = informative_pairs(rt, rn)
    E = [(a, b) for a, b in tp if a['top'] != b['top'] or a['bottom'] != b['bottom']]
    op = informative_pairs(ro, rt)
    C = [(a, b) for a, b in op if a['top'] != b['top'] or a['bottom'] != b['bottom']]
    def pl(v, ka, kb):
        return ' (first: ' + ' '.join('%s%d=%s%d' % (ka, a['present'], kb, b['present']) for a, b in v[:10]) + ')' if v else ''
    lines = ['  TALL %d presents, no-cull %d, control (H2 off) %d' % (len(rt), len(rn), len(ro)),
             '  TALL against no-cull: %d informative pairs (TALL presents %s), with different strips (E) %d%s'
             % (len(tp), ('%d..%d' % (tp[0][0]['present'], tp[-1][0]['present'])) if tp else '-', len(E), pl(E, 'T', 'N')),
             '  control against TALL: %d informative pairs, with different strips (C: H2 visibly changes the strips) %d%s' % (len(op), len(C), pl(C, 'O', 'T'))]
    if len(tp) < min_pairs:
        lines.append('  => FAIL (only %d TALL / no-cull pairs, at least %d needed: the runs never showed the same moment)' % (len(tp), min_pairs))
        return 1, lines
    if not E and C:
        lines.append('  => PASS (H2 shows content the 16:9 cull removes at %d moments, and the strips equal the no-cull reference at all %d)' % (len(C), len(tp)))
        return 0, lines
    if not E:
        lines.append('  => NOT DEMONSTRATED (%s): TALL = no-cull here, but without H2 the strips would be the same, so this says nothing about H2'
                     % ('the control equals TALL at all %d shared moments: the window holds nothing the 16:9 cull removes from the strips' % len(op) if len(op) >= min_pairs
                        else 'the control shared only %d moments with TALL' % len(op)))
        return 3, lines
    if not C:
        if len(op) < min_pairs:
            lines.append('  => NOT DEMONSTRATED (TALL differs from the no-cull reference at %d moments, and the control shared only %d moments with TALL: H2 is not shown at work)' % (len(E), len(op)))
            return 3, lines
        lines.append('  => FAIL (H2 changes nothing visible at the %d moments the control shares with TALL, yet the no-cull reference shows more in the strips at %d moments: H2 leaves out what the reference shows)' % (len(op), len(E)))
        return 1, lines
    cp = sorted(set(b['present'] for a, b in C))
    far = [(a, b) for a, b in E if min(abs(a['present'] - p) for p in cp) > H2_NEAR]
    lines.append('  TALL presents where H2 visibly works (C) %d..%d; TALL / no-cull differences more than %d presents from all of them: %d%s'
                 % (cp[0], cp[-1], H2_NEAR, len(far), pl(far, 'T', 'N')))
    if far:
        lines.append('  => FAIL (TALL differs from the no-cull reference at %d moments where H2 is not seen at work: content H2 leaves out)' % len(far))
        return 1, lines
    lines.append('  => NOT EXACT (H2 visibly changes the strips at %d moments, and TALL differs from the no-cull reference at %d moments, all of them where H2 works: '
                 'an effect drawn in another state by the reference (its life runs earlier there: measured in the task-40 review) or content H2 leaves out; '
                 'the band log cannot tell them apart: judge the dumps)' % (len(C), len(E)))
    return 4, lines


def read_hashlog(path):
    """KHNMC_GFX_HASHLOG -> list of (present, game_frame, rgb_hash, draws_total)"""
    rows = []
    with open(path) as f:
        head = f.readline().rstrip('\n').split('\t')
        if head[:6] != ['present', 'game_frame', 'vblank', 'rgb_hash', 'bgra_hash', 'draws_total']:
            raise SystemExit('fill_compare: %s: not a KHNMC_GFX_HASHLOG file' % path)
        for ln in f:
            c = ln.rstrip('\n').split('\t')
            if len(c) >= 6:
                rows.append((int(c[0]), int(c[1]), c[3], int(c[5])))
    return rows


def hashprefix(ref, new, min_presents=970):
    """16:9 pictures unchanged (task 40 review finding 2): two runs of one screen size (nothing set / KHNMC_ASPECT=fill) are compared
    by present number ONLY while they are the same run: up to the first present whose cumulative draw count differs (from there the
    GPU runs may show different moments; that is not a picture of the fill mode).  -> (code, lines): 0 PASS (every picture before that
    present is equal and the prefix reaches min_presents: on the census route 970 covers the title, the menus and the map-transition
    fade 880-970), 1 FAIL (a picture differs while the draw counts are still equal), 3 UNDECIDED (all equal, but the runs' draw counts
    part before min_presents: the 16:9 identity then rests on the state proof, runcheck fill169)."""
    a = {r[0]: r for r in ref}; b = {r[0]: r for r in new}
    common = sorted(set(a) & set(b))
    d = next((p for p in common if a[p][3] != b[p][3]), None)
    upto = [p for p in common if d is None or p < d]
    diff = [p for p in upto if a[p][2] != b[p][2]]
    last = upto[-1] if upto else 0
    lines = ['  REF %d presents, NEW %d; the draw counts first differ at present %s; presents compared by number before that: %d (up to present %d); pictures different there: %d%s'
             % (len(ref), len(new), d if d is not None else 'never (equal in all %d shared presents)' % len(common), len(upto), last, len(diff),
                (' (first: ' + ' '.join('%d/gf%d' % (p, b[p][1]) for p in diff[:10]) + ')') if diff else '')]
    if diff:
        lines.append('  => FAIL (fill changed a picture while both runs drew the same: or the runs diverged without a draw-count change; a rerun tells)')
        return 1, lines
    if last < min_presents:
        lines.append('  => UNDECIDED (all %d pictures equal, but the draw counts part at present %s, before the minimum %d: the runs became different moments; '
                     'the 16:9 identity then rests on runcheck fill169 (every hook wrote nothing))' % (len(upto), d, min_presents))
        return 3, lines
    lines.append('  => PASS (every picture equal by present number through present %d, at least %d)' % (last, min_presents))
    return 0, lines


def hashcontent(ref, new, min_share=0.99):
    n = 0
    for x, y in zip(ref, new):
        if x[2] != y[2]:
            break
        n += 1
    seen = set(r[2] for r in ref)
    miss = [r for r in new if r[2] not in seen]
    share = (len(new) - len(miss)) / float(len(new)) if new else 0.0
    ok = bool(new) and share >= min_share
    return ok, ['  REF %d presents, NEW %d; equal by present number: the first %d; by content: %d of %d NEW pictures also in REF (share %.4f, at least %g): %s%s'
                % (len(ref), len(new), n, len(new) - len(miss), len(new), share, min_share, 'PASS' if ok else 'FAIL',
                   (' (NEW presents not in REF: ' + ' '.join('%d/gf%d' % (r[0], r[1]) for r in miss[:10]) + (' ...' if len(miss) > 10 else '') + ')') if miss else '')]


H2_RE = re.compile(r'^kh: aspect report: (H2[bc]) \(0x[0-9a-f]+, [^)]*\): calls (\d+): r = 1, nothing written (\d+); m11 divided by ([^)]*?)(?: \(TEST AID no-cull\))? and restored (\d+); test aid off (\d+); changed inside the call \(game.s value kept\) (\d+); camera not guest memory (\d+)$')
H4_RE = re.compile(r'^kh: aspect report: H4 \(fades and event bars\): f_156e20 calls (\d+); r = 1, nothing written (\d+); test aid off (\d+);.*; totals: rewritten (\d+) \+ (\d+), already (\d+), other rect (\d+) \+ (\d+)$')
H5_RE = re.compile(r'^kh: aspect report: H5 .*: calls (\d+): extended (\d+), other rects \(left\) (\d+), r = 1 (\d+), test aid off (\d+)$')
AID_RE = re.compile(r'^kh: aspect: TEST AID: (.*)\(not for play\)$')
OFF_NAMES = {'h2': ('H2b', 'H2c'), 'h2b': ('H2b',), 'h2c': ('H2c',), 'h4': ('H4',), 'h5': ('H5',)}


def fillcheck(run_out, role, screen, offlist=None):
    """runcheck roles (task 40; the test-aid roles from the task-40 review, finding 1):
      fill     the mode as played: KHNMC_ASPECT=fill, NO test aid at all (no `TEST AID` line, H2 dividing by r, nothing passed through)
      fill169  KHNMC_ASPECT=fill on a 16:9 screen: no narrow H1 call, every H2b / H2c call "r = 1, nothing written", H4 / H5 never wrote
      nocull   fill + KHNMC_ASPECT_TEST_NOCULL=1: the TEST AID line says so, H2b / H2c divided by 2047/208 at every call (none off)
      off      fill + KHNMC_ASPECT_TEST_OFF=offlist: the TEST AID line names exactly those hooks; each of them passed EVERY call through
               and wrote nothing (H2: test aid off = calls, divided 0; H4: test aid off > 0, rewritten 0; H5: test aid off = calls);
               the others at work as in fill.
    -> (ok, lines)"""
    try:
        text = open(run_out, 'rb').read().decode('utf-8', 'replace').splitlines()
    except OSError as e:
        return False, ['  run.out %s: cannot be read (%s): FAIL' % (run_out, e)]
    lines, ok = [], True
    def say(good, what):
        nonlocal ok
        ok = ok and good
        lines.append('  %s: %s' % (what, 'PASS' if good else 'FAIL'))
    tall = role in ('fill', 'nocull', 'off')
    off = set()
    for k in (offlist or []):
        off.update(OFF_NAMES[k])
    aid = [m.group(1) for m in (AID_RE.match(l) for l in text) if m]
    want_aid = ''.join('%s off; ' % k for k in ('H2b', 'H2c', 'H4', 'H5') if k in off) + ('H2 divides m11 by 2047/208 instead of r (the no-cull reference); ' if role == 'nocull' else '')
    if role in ('fill', 'fill169'):
        say(not aid, '%s: no TEST AID line (%s)' % (role, ('found: ' + aid[-1].strip()) if aid else 'none'))
    else:
        say(bool(aid) and aid[-1] == want_aid, '%s: the TEST AID line is "%s" (expected "%s")' % (role, aid[-1].strip() if aid else '(none: the test aid did not take effect)', want_aid.strip()))
    h1 = [m for m in (H1_RE.match(l) for l in text) if m]
    if not h1:
        say(False, '%s: no H1 report line' % role)
    else:
        m = h1[-1]; mode, uncond = m.group(1), bool(m.group(2)); calls, wide, narrow, rew, kept, mism = (int(m.group(i)) for i in (3, 4, 5, 6, 7, 8))
        if tall:
            say(mode == 'fill' and not uncond and calls > 0 and rew > 0 and mism == 0, '%s: H1: KHNMC_ASPECT=%s%s, calls %d, rewritten %d, different from the model %d (fill without the H1 test aid, at least one rewrite, none different)' % (role, mode, ' + unconditional' if uncond else '', calls, rew, mism))
        else:
            say(mode == 'fill' and not uncond and calls > 0 and narrow == 0, '%s: H1: KHNMC_ASPECT=%s, calls %d, narrow %d (fill on a 16:9 screen: none narrow)' % (role, mode, calls, narrow))
    h2 = {m.group(1): m for m in (H2_RE.match(l) for l in text) if m}
    for k in ('H2b', 'H2c'):
        if k not in h2:
            say(False, '%s: no %s report line (the build lacks task 40)' % (role, k)); continue
        m = h2[k]; calls, ident, edited, offn, inside, bad = (int(m.group(i)) for i in (2, 3, 5, 6, 7, 8)); nocull = 'TEST AID' in m.group(0)
        desc = '%s: %s: calls %d, divided%s and restored %d, test aid off %d, changed inside %d, camera not guest memory %d' % (role, k, calls, ' by 2047/208 (NO-CULL test aid)' if nocull else ' by r', edited, offn, inside, bad)
        if role == 'fill169':
            say(calls > 0 and ident == calls and edited == 0, '%s: %s: calls %d, r = 1 nothing written %d, edited %d (all calls must be r = 1)' % (role, k, calls, ident, edited))
        elif k in off:
            say(calls > 0 and offn == calls and edited == 0 and not nocull, desc + ' (the test aid passes every call through)')
        else:
            say(calls > 0 and edited == calls and offn == 0 and inside == 0 and bad == 0 and nocull == (role == 'nocull'),
                desc + (' (every call divided by 2047/208)' if role == 'nocull' else ' (every call divided by r, no test aid)'))
    h4 = [m for m in (H4_RE.match(l) for l in text) if m]
    h5 = [m for m in (H5_RE.match(l) for l in text) if m]
    if not h4 or not h5:
        say(False, '%s: no H4 / H5 report line' % role)
    else:
        h4off, r1, r2 = int(h4[-1].group(3)), int(h4[-1].group(4)), int(h4[-1].group(5)); h5calls, ext, h5off = int(h5[-1].group(1)), int(h5[-1].group(2)), int(h5[-1].group(5))
        if role == 'fill169':
            say(r1 + r2 == 0 and ext == 0, '%s: H4 rewritten %d + %d, H5 extended %d (none at 16:9)' % (role, r1, r2, ext))
        elif 'H4' in off:
            say(h4off > 0 and r1 + r2 == 0, '%s: H4 test aid off %d, rewritten %d + %d (the test aid passes every call through)' % (role, h4off, r1, r2))
        else:
            say(r1 + r2 > 0 and h4off == 0, '%s: H4 rewritten %d at the draw + %d after f_156e20, test aid off %d (at least one rewrite, no test aid)' % (role, r1, r2, h4off))
        if tall:
            say(h5off == (h5calls if 'H5' in off else 0), '%s: H5 calls %d, test aid off %d (%s)' % (role, h5calls, h5off, 'all' if 'H5' in off else 'none'))
    scr = [m for m in (SCREEN_RE.match(l) for l in text) if m]
    if screen:
        if not scr:
            say(False, '%s: no `kh: screen: KHNMC_SCREEN=...` exit-report line' % role)
        else:
            m = scr[-1]; w, h, np_, nb = (int(m.group(i)) for i in (1, 2, 3, 4))
            say((w, h) == screen and np_ > 0 and nb == 0, '%s: screen %dx%d (expected %dx%d), checked at %d presents, not the screen at %d' % (role, w, h, screen[0], screen[1], np_, nb))
    warn = [l for l in text if 'WARNING:' in l and ('kh: screen' in l or 'kh: aspect' in l)]
    say(not warn, '%s: aspect / screen WARNING lines: %d%s' % (role, len(warn), (' (first: %s)' % warn[0][:200]) if warn else ''))
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
    # task 40: tallcheck, bandpairs, hashcontent and the fill / fill169 run checks on synthetic logs
    BH = 'present\tgame_frame\twidth\theight\tband_top_row\tband_rows\tband_hash\ttop_hash\tbottom_hash\tband_nonblack_pct\ttop_nonblack_pct\tbottom_nonblack_pct\tband_mean\ttop_mean\tbottom_mean\tedge_top\tedge_bottom\tband_step_p95\n'
    def brow(p, band, top, bot, bn, tn, on, et=2.0, eb=2.0, p95=6.0, w=1920, h=1350):
        if top is None:
            return '%d\t%d\t%d\t%d\t0\t%d\t%s\t-\t-\t%.4f\t-\t-\t1\t-\t-\t-\t-\t%.4f\n' % (p, p, w, h, h, band, bn, p95)
        return '%d\t%d\t%d\t%d\t135\t1080\t%s\t%s\t%s\t%.4f\t%.4f\t%.4f\t1\t1\t1\t%.4f\t%.4f\t%.4f\n' % (p, p, w, h, band, top, bot, bn, tn, on, et, eb, p95)
    good = [brow(i, 'b%d' % i, 't%d' % i, 'o%d' % i, 95.0, 97.0, 80.0) for i in range(1, 30)] + [brow(30 + i, 'black', 'tb', 'ob', 0.0, 0.0, 0.0) for i in range(5)]
    def bw(r, etw=1.0, ebw=1.0):                      # a row of the band log with the task-40-review window columns
        return r.rstrip('\n') + ('\t6\t%.4f\t%.4f\n' % (etw, ebw) if r.split('\t')[7] != '-' else '\t-\t-\t-\n')
    tc_cases = [('tallcheck: lit frames and full fades over the whole frame', [bw(r) for r in good], True),
                ('tallcheck: a fade over the band only (strips lit)', [bw(r) for r in good] + [bw(brow(40, 'black', 'tz', 'oz', 0.0, 97.0, 82.0))], False),
                ('tallcheck: a partial fade over the band only (hard line)', [bw(r) for r in good] + [bw(brow(41, 'bx', 'ty', 'oy', 60.0, 97.0, 82.0, et=48.0, eb=50.0, p95=4.0), 48.0, 50.0)], False),
                ('tallcheck: no picture narrower than 16:9', [bw(brow(1, 'b', None, None, 95.0, None, None, w=1920, h=1080))], False)]
    pa = [brow(i, 'b%d' % i, 't%d' % i, 'o%d' % i, 95.0, 97.0, 80.0) for i in range(1, 30)] + [brow(40, 'black', 'tb', 'ob', 0.0, 0.0, 0.0)]
    pb = [brow(i + 1, 'b%d' % i, 't%d' % i, 'o%d' % i, 95.0, 97.0, 80.0) for i in range(1, 30)] + [brow(41, 'black', 'tq', 'oq', 0.0, 0.0, 0.0)]  # shifted by one present
    pb_hole = list(pb); pb_hole[10] = brow(12, 'b11', 't11-more', 'o11', 95.0, 97.0, 80.0)
    bp_cases = [('bandpairs: 29 pairs shifted by one present, equal strips', pa, pb, True),
                ('bandpairs: one pair with a different top strip', pa, pb_hole, False),
                ('bandpairs: only 5 pairs', pa[:5], pb[:5], False)]
    hc = 'present\tgame_frame\tvblank\trgb_hash\tbgra_hash\tdraws_total\twidth\theight\n'
    hr = [hc] + ['%d\t%d\t0\th%d\tx\t0\t1920\t1080\n' % (i, i, i // 2) for i in range(1, 201)]
    hn_shift = [hc] + ['%d\t%d\t0\th%d\tx\t0\t1920\t1080\n' % (i, i, (i + 1) // 2) for i in range(1, 201)]
    hn_new = [hc] + ['%d\t%d\t0\th%s\tx\t0\t1920\t1080\n' % (i, i, ('new%d' % i) if i % 20 == 0 else str(i // 2)) for i in range(1, 201)]
    hc_cases = [('hashcontent: the same pictures', hr, hr, True), ('hashcontent: the same pictures one present later', hr, hn_shift, True),
                ('hashcontent: 5 %% of the pictures new', hr, hn_new, False)]
    def h2l(k, calls, ident, edited, inside=0, nocull=False):
        return 'kh: aspect report: %s (0x%s, %s pass): calls %d: r = 1, nothing written %d; m11 divided by %s and restored %d; test aid off 0; changed inside the call (game\'s value kept) %d; camera not guest memory 0' % (
            k, '186920' if k == 'H2b' else '2bd190', 'map' if k == 'H2b' else 'effect', calls, ident, '2047/208 (TEST AID no-cull)' if nocull else 'r', edited, inside)
    def fl(unc, narrow, rew, e2, ident, h4, ext=0, scr_=(1920, 1350)):
        return (scr(scr_[0], scr_[1], 2406, 0) if scr_ else []) + [h1('fill', unc, 2400, narrow, rew, 2400 - rew), h2l('H2b', 2000, ident, e2), h2l('H2c', 2000, ident, e2),
                'kh: aspect report: H4 (fades and event bars): f_156e20 calls 4800; r = 1, nothing written 0; test aid off 0; [x]; totals: rewritten %d + %d, already 0, other rect 0 + 0' % (h4, h4),
                'kh: aspect report: H5 (full-height scissors, 0x126fd0): calls 900: extended %d, other rects (left) 900, r = 1 0, test aid off 0' % ext]
    fc_cases = [('runcheck fill: gated fill, H2 and H4 at work', 'fill', (1920, 1350), fl(False, 2400, 2100, 2000, 0, 3000), True),
                ('runcheck fill: the H1 test aid on', 'fill', (1920, 1350), fl(True, 2400, 2400, 2000, 0, 3000), False),
                ('runcheck fill: H2 never edited', 'fill', (1920, 1350), fl(False, 2400, 2100, 0, 0, 3000), False),
                ('runcheck fill: H4 never rewrote', 'fill', (1920, 1350), fl(False, 2400, 2100, 2000, 0, 0), False),
                ('runcheck fill: an aspect WARNING', 'fill', (1920, 1350), fl(False, 2400, 2100, 2000, 0, 3000) + ['kh: aspect: WARNING: H2b (0x186920, map pass): cam+0x98 ...'], False),
                ('runcheck fill169: fill at 16:9, nothing written', 'fill169', None, fl(False, 0, 0, 0, 2000, 0, scr_=None), True),
                ('runcheck fill169: H2 edited at 16:9', 'fill169', None, fl(False, 0, 0, 5, 1995, 0, scr_=None), False),
                ('runcheck fill169: H5 extended at 16:9', 'fill169', None, fl(False, 0, 0, 0, 2000, 0, ext=3, scr_=None), False)]
    with tempfile.TemporaryDirectory() as td:
        def wr(name, rows, head=BH):
            p = os.path.join(td, name)
            with open(p, 'w') as f:
                f.write(head + ''.join(rows))
            return p
        for k, (name, rows, want) in enumerate(tc_cases):
            ok, out = tallcheck(read_bandlog(wr('tc%d.tsv' % k, rows, BH.rstrip('\n') + '\tedge_window_rows\tedge_top_max\tedge_bottom_max\n')))
            good_ = ok == want; n += 1; fails += 0 if good_ else 1
            print('fill_compare selftest: %-58s %s (expected %s)' % (name, 'PASS' if ok else 'FAIL', 'PASS' if want else 'FAIL') + ('' if good_ else '   <-- WRONG\n' + '\n'.join(out)))
        for k, (name, a, b, want) in enumerate(bp_cases):
            ok, out = bandpairs(read_bandlog(wr('a%d.tsv' % k, a)), read_bandlog(wr('b%d.tsv' % k, b)))
            good_ = ok == want; n += 1; fails += 0 if good_ else 1
            print('fill_compare selftest: %-58s %s (expected %s)' % (name, 'PASS' if ok else 'FAIL', 'PASS' if want else 'FAIL') + ('' if good_ else '   <-- WRONG\n' + '\n'.join(out)))
        for k, (name, a, b, want) in enumerate(hc_cases):
            pa_, pb_ = os.path.join(td, 'hr%d.tsv' % k), os.path.join(td, 'hn%d.tsv' % k)
            open(pa_, 'w').write(''.join(a)); open(pb_, 'w').write(''.join(b))
            ok, out = hashcontent(read_hashlog(pa_), read_hashlog(pb_))
            good_ = ok == want; n += 1; fails += 0 if good_ else 1
            print('fill_compare selftest: %-58s %s (expected %s)' % (name, 'PASS' if ok else 'FAIL', 'PASS' if want else 'FAIL') + ('' if good_ else '   <-- WRONG\n' + '\n'.join(out)))
        for k, (name, role, sc, lines_, want) in enumerate(fc_cases):
            p = os.path.join(td, 'f%02d.out' % k)
            open(p, 'w').write('\n'.join(lines_) + '\n')
            ok, out = fillcheck(p, role, sc)
            good_ = ok == want; n += 1; fails += 0 if good_ else 1
            print('fill_compare selftest: %-58s %s (expected %s)' % (name, 'PASS' if ok else 'FAIL', 'PASS' if want else 'FAIL') + ('' if good_ else '   <-- WRONG\n' + '\n'.join(out)))
    # task 40 review: findings 1 (h2pictures, informative pairs, the test-aid run roles), 2 (hashprefix), 3 (the edge windows)
    BH2 = BH.rstrip('\n') + '\tedge_window_rows\tedge_top_max\tedge_bottom_max\n'
    def brow2(p, band, top, bot, bn, tn, on, et=0.5, eb=0.5, p95=6.0, etw=1.0, ebw=1.0):
        return brow(p, band, top, bot, bn, tn, on, et, eb, p95).rstrip('\n') + '\t6\t%.4f\t%.4f\n' % (etw, ebw)
    good2 = [brow2(i, 'b%d' % i, 't%d' % i, 'o%d' % i, 95.0, 97.0, 80.0) for i in range(1, 30)] + [brow2(30 + i, 'black', 'tb', 'ob', 0.0, 0.0, 0.0, 0, 0, 0, 0, 0) for i in range(5)]
    # run-Z40/gpu/bl-off/r01 present 952 (the fade-in without H4): the one-row edges 0.63 / 0.88, p95 4.39 (old (s) passed it); the
    # windows from run-Z40/png/fade-in-1920x1350-H4-off-p952.png: 16.95 (rows 131|132) / 24.69 (rows 1217|1218)
    p952 = brow2(952, 'bfade', 'tlit', 'olit', 95.48, 95.55, 81.93, 0.6306, 0.8757, 4.392, 16.95, 24.69)
    # field-1920x1350-fill-H2-p1020.png: the command menu's edge at row 1210 lies inside the band; the windows hold 1.60 / 1.46
    p1020 = brow2(1020, 'bfield', 'tf', 'of', 96.0, 97.0, 82.0, 1.41, 1.40, 8.26, 1.60, 1.46)
    tc2 = [('tallcheck: lit frames and full fades (window columns)', good2, True),
           ('tallcheck: the bl-off fade-in present 952 (seam 3 rows out)', good2 + [p952], False),
           ('tallcheck: a lit field present with a HUD edge in the band', good2 + [p1020], True)]
    def aon(k, unc=False):
        return [] if k is None else ['kh: aspect: TEST AID: %s(not for play)' % k]
    def h4l(off, rew):
        return 'kh: aspect report: H4 (fades and event bars): f_156e20 calls 4800; r = 1, nothing written 0; test aid off %d; [x]; totals: rewritten %d + %d, already 0, other rect 0 + 0' % (off, rew, rew)
    def h5l(off):
        return 'kh: aspect report: H5 (full-height scissors, 0x126fd0): calls 900: extended 0, other rects (left) 900, r = 1 0, test aid off %d' % off
    def h2x(k, edited, offn, nocull=False):
        return 'kh: aspect report: %s (0x%s, %s pass): calls 2000: r = 1, nothing written 0; m11 divided by %s and restored %d; test aid off %d; changed inside the call (game\'s value kept) 0; camera not guest memory 0' % (
            k, '186920' if k == 'H2b' else '2bd190', 'map' if k == 'H2b' else 'effect', '2047/208 (TEST AID no-cull)' if nocull else 'r', edited, offn)
    def tl(aid, b, c, h4off, h4rew, h5off=0, nocull=False):
        return scr(1920, 1350, 2406, 0) + aon(aid) + [h1('fill', False, 2400, 2400, 2100, 300), h2x('H2b', *b, nocull=nocull), h2x('H2c', *c, nocull=nocull), h4l(h4off, h4rew), h5l(h5off)]
    NC = 'H2 divides m11 by 2047/208 instead of r (the no-cull reference); '
    rc2 = [('runcheck fill: the mode as played', 'fill', None, tl(None, (2000, 0), (2000, 0), 0, 3000), True),
           ('runcheck fill: a no-cull run passed off as the tall run', 'fill', None, tl(NC, (2000, 0), (2000, 0), 0, 3000, nocull=True), False),
           ('runcheck fill: an H2-off run passed off as the tall run', 'fill', None, tl('H2b off; H2c off; ', (0, 2000), (0, 2000), 0, 3000), False),
           ('runcheck nocull: the no-cull reference', 'nocull', None, tl(NC, (2000, 0), (2000, 0), 0, 3000, nocull=True), True),
           ('runcheck nocull: the test aid did not take effect', 'nocull', None, tl(None, (2000, 0), (2000, 0), 0, 3000), False),
           ('runcheck off h2: H2 off', 'off', ['h2'], tl('H2b off; H2c off; ', (0, 2000), (0, 2000), 0, 3000), True),
           ('runcheck off h2: the test aid did not take effect', 'off', ['h2'], tl(None, (2000, 0), (2000, 0), 0, 3000), False),
           ('runcheck off h2: H2c still edits', 'off', ['h2'], tl('H2b off; H2c off; ', (0, 2000), (2000, 0), 0, 3000), False),
           ('runcheck off h4: H4 off', 'off', ['h4'], tl('H4 off; ', (2000, 0), (2000, 0), 4800, 0), True),
           ('runcheck off h4: H4 still rewrote', 'off', ['h4'], tl('H4 off; ', (2000, 0), (2000, 0), 4800, 7), False),
           ('runcheck off h4: the H2 test aid on too', 'off', ['h4'], tl('H2b off; H2c off; H4 off; ', (0, 2000), (0, 2000), 4800, 0), False)]
    # h2pictures: T = TALL, N = no-cull, O = H2 off; band states s1..s30 lit; at s7 and s8 the 16:9 cull removes something from the top strip
    def hrows(seq, cull=False, shift=0):
        return [brow2(i + shift, 's%d' % i, ('t%d-cut' % i) if cull and i in (7, 8) else 't%d' % i, 'o%d' % i, 95.0, 97.0, 80.0) for i in seq]
    bars = [brow2(i, 'title%d' % i, 'tb', 'ob', 50.0, 0.0, 0.0) for i in range(100, 140)]     # the exe's bars: black strips
    hstate = [r.replace('\tt9\t', '\tt9-state\t') for r in hrows(range(1, 31))]                    # the reference draws an effect in another state at s9
    hfar = [r.replace('\tt25\t', '\tt25-more\t') for r in hrows(range(1, 31))]                     # the reference shows more at s25, far from s7/s8
    hp_cases = [('h2pictures: control differs at s7/s8, TALL = no-cull there', hrows(range(1, 31)), hrows(range(1, 31), shift=2), hrows(range(1, 31), True, 1), 0),
                ('h2pictures: the control never differs (the bl-* window)', hrows(range(1, 31)), hrows(range(1, 31)), hrows(range(1, 31)), 3),
                ('h2pictures: control differs only where TALL never was', hrows([i for i in range(1, 31) if i not in (7, 8)]), hrows(range(1, 31)), hrows(range(1, 31), True), 3),
                ('h2pictures: TALL lacks the strip content (H2 not working)', hrows(range(1, 31), True), hrows(range(1, 31)), hrows(range(1, 31), True), 1),
                ('h2pictures: 40 pairs, all title bars: too few informative', bars + hrows(range(1, 5)), bars + hrows(range(1, 5)), bars + hrows(range(1, 5), True), 1),
                ('h2pictures: H2 at work, reference differs at s9 (st-* window)', hrows(range(1, 31)), hstate, hrows(range(1, 31), True), 4),
                ('h2pictures: H2 at work at s7/s8, reference differs at s25', hrows(range(1, 31)), hfar, hrows(range(1, 31), True), 1),
                ('h2pictures: TALL differs, control shares only 4 moments', hrows(range(1, 31)), hstate, hrows(range(1, 5), True), 3)]
    hl = 'present\tgame_frame\tvblank\trgb_hash\tbgra_hash\tdraws_total\twidth\theight\n'
    def hlog(n, div=None, pic_at=None, newpic=0):
        out = [hl]; d = 0
        for i in range(1, n + 1):
            d += 400 if div is None or i < div else 401                                   # from `div` on the runs draw differently
            pic = ('x%d' % i) if (div is not None and i >= div) or i == pic_at else 'p%d' % (i // 3)
            if newpic and i >= div and i % 5:
                pic = 'n%d' % i                                                          # after the divergence: moments REF never showed
            out.append('%d\t%d\t0\t%s\tz\t%d\t1920\t1080\n' % (i, i, pic, d))
        return out
    ref_h = hlog(2400)
    hx_cases = [('hashprefix: the same run (2400 presents equal)', ref_h, ref_h, 970, 0),
                ('hashprefix: the runs part at 1500 (after the fade)', ref_h, hlog(2400, div=1500, newpic=1), 970, 0),
                ('hashprefix: the runs part at 960 (reviewer): not a failure', ref_h, hlog(2400, div=960, newpic=1), 970, 3),
                ('hashprefix: a picture differs at 500, draws equal', ref_h, hlog(2400, pic_at=500), 970, 1)]
    with tempfile.TemporaryDirectory() as td:
        def wr2(name, rows, head):
            p = os.path.join(td, name)
            with open(p, 'w') as f:
                f.write(head + ''.join(rows))
            return p
        try:
            tallcheck(read_bandlog(wr2('old.tsv', good, BH)))
            ok_old = False
        except BandlogTooOld:
            ok_old = True
        n += 1; fails += 0 if ok_old else 1
        print('fill_compare selftest: %-58s %s' % ('tallcheck: an old band log is a named error', 'PASS' if ok_old else 'WRONG (no error)'))
        for k, (name, rows, want) in enumerate(tc2):
            ok, out = tallcheck(read_bandlog(wr2('tc2_%d.tsv' % k, rows, BH2)))
            good_ = ok == want and (want or any(l.startswith('  (s)') and l.endswith('FAIL (a seam at the band edge: a partial fade or bars over the band only)') for l in out))
            n += 1; fails += 0 if good_ else 1
            print('fill_compare selftest: %-58s %s (expected %s)' % (name, 'PASS' if ok else 'FAIL', 'PASS' if want else 'FAIL on (s)') + ('' if good_ else '   <-- WRONG\n' + '\n'.join(out)))
        for k, (name, role, offl, lines_, want) in enumerate(rc2):
            p = wr2('rc2_%02d.out' % k, ['\n'.join(lines_) + '\n'], '')
            ok, out = fillcheck(p, role, (1920, 1350), offl)
            good_ = ok == want; n += 1; fails += 0 if good_ else 1
            print('fill_compare selftest: %-58s %s (expected %s)' % (name, 'PASS' if ok else 'FAIL', 'PASS' if want else 'FAIL') + ('' if good_ else '   <-- WRONG\n' + '\n'.join(out)))
        for k, (name, t, nn, o, want) in enumerate(hp_cases):
            code, out = h2pictures(read_bandlog(wr2('ht%d.tsv' % k, t, BH2)), read_bandlog(wr2('hn%d.tsv' % k, nn, BH2)), read_bandlog(wr2('ho%d.tsv' % k, o, BH2)))
            good_ = code == want; n += 1; fails += 0 if good_ else 1
            print('fill_compare selftest: %-58s %d (expected %d)' % (name, code, want) + ('' if good_ else '   <-- WRONG\n' + '\n'.join(out)))
        for k, (name, a, b, mnp, want) in enumerate(hx_cases):
            code, out = hashprefix(read_hashlog(wr2('xa%d.tsv' % k, a[1:], a[0])), read_hashlog(wr2('xb%d.tsv' % k, b[1:], b[0])), mnp)
            good_ = code == want; n += 1; fails += 0 if good_ else 1
            print('fill_compare selftest: %-58s %d (expected %d)' % (name, code, want) + ('' if good_ else '   <-- WRONG\n' + '\n'.join(out)))
        # the reviewer's arithmetic: the old 99 % share fails the run that parts at 960
        okc, _ = hashcontent(read_hashlog(wr2('xa_c.tsv', ref_h[1:], ref_h[0])), read_hashlog(wr2('xb_c.tsv', hx_cases[2][2][1:], hl)))
        n += 1; fails += 0 if not okc else 1
        print('fill_compare selftest: %-58s %s (expected FAIL: why it is no longer a condition)' % ('hashcontent on the run that parts at 960', 'PASS' if okc else 'FAIL'))
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
    out = opt('--out'); png = opt('--png'); mp = opt('--min-pairs'); ms = opt('--min-share')
    fs = opt('--fade-strip', float)
    if fs is not None:
        th['fade_strip'] = fs
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
    mn = opt('--min-presents')
    if av[0] in ('tallcheck', 'bandpairs', 'hashcontent', 'runcheck', 'h2pictures', 'hashprefix'):
        for f in av[1:{'bandpairs': 3, 'hashcontent': 3, 'hashprefix': 3, 'h2pictures': 4}.get(av[0], 2)]:
            if not os.path.isfile(f):
                print('fill_compare: %s: %s is missing' % (av[0], f), file=sys.stderr); return 2
    if av[0] == 'tallcheck' and len(av) == 2:
        try:
            ok, out = tallcheck(read_bandlog(av[1]), th.get('fade_strip', 0.01), th['edge_ratio'])
        except BandlogTooOld as e:
            print(str(e), file=sys.stderr); return 2
        print('fill_compare: tallcheck %s: %s' % (av[1], 'PASS' if ok else 'FAIL')); print('\n'.join(out)); return 0 if ok else 1
    if av[0] == 'h2pictures' and len(av) == 4:
        code, out = h2pictures(read_bandlog(av[1]), read_bandlog(av[2]), read_bandlog(av[3]), int(mp) if mp else 20)
        print('fill_compare: h2pictures %s %s %s: %s' % (av[1], av[2], av[3], {0: 'PASS', 1: 'FAIL', 3: 'NOT DEMONSTRATED', 4: 'NOT EXACT'}[code])); print('\n'.join(out)); return code
    if av[0] == 'hashprefix' and len(av) == 3:
        code, out = hashprefix(read_hashlog(av[1]), read_hashlog(av[2]), int(mn) if mn else 970)
        print('fill_compare: hashprefix %s %s: %s' % (av[1], av[2], {0: 'PASS', 1: 'FAIL', 3: 'UNDECIDED'}[code])); print('\n'.join(out)); return code
    if av[0] == 'bandpairs' and len(av) == 3:
        ok, out = bandpairs(read_bandlog(av[1]), read_bandlog(av[2]), int(mp) if mp else 20)
        print('fill_compare: bandpairs %s %s: %s' % (av[1], av[2], 'PASS' if ok else 'FAIL')); print('\n'.join(out)); return 0 if ok else 1
    if av[0] == 'hashcontent' and len(av) == 3:
        ok, out = hashcontent(read_hashlog(av[1]), read_hashlog(av[2]), float(ms) if ms else 0.99)
        print('fill_compare: hashcontent %s %s: %s' % (av[1], av[2], 'PASS' if ok else 'FAIL')); print('\n'.join(out)); return 0 if ok else 1
    if av[0] == 'runcheck' and len(av) in (3, 4, 5) and av[2] in ('fill', 'fill169', 'nocull', 'off'):
        sc = None
        if len(av) >= 4:
            m = re.fullmatch(r'(\d+)x(\d+)', av[3])
            if not m:
                print('fill_compare: runcheck: the screen must be WxH, not %s' % av[3], file=sys.stderr); return 2
            sc = (int(m.group(1)), int(m.group(2)))
        if av[2] in ('fill', 'nocull', 'off') and not sc:
            print('fill_compare: runcheck RUN.OUT %s needs the screen (WxH)' % av[2], file=sys.stderr); return 2
        offl = None
        if av[2] == 'off':
            offl = av[4].split(',') if len(av) == 5 else []
            if not offl or any(k not in OFF_NAMES for k in offl):
                print('fill_compare: runcheck RUN.OUT off WxH LIST: LIST is the KHNMC_ASPECT_TEST_OFF value (h2, h2b, h2c, h4, h5, comma-separated)', file=sys.stderr); return 2
        elif len(av) == 5:
            print(__doc__); return 2
        ok, out = fillcheck(av[1], av[2], sc, offl)
        print('fill_compare: runcheck %s (%s%s): %s' % (av[1], av[2], ' ' + ','.join(offl) if offl else '', 'PASS' if ok else 'FAIL')); print('\n'.join(out)); return 0 if ok else 1
    if av[0] == 'selftest' and len(av) == 1:
        return selftest()
    print(__doc__); return 2


if __name__ == '__main__':
    sys.exit(main())
