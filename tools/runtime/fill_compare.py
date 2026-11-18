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
  task 42 (H3, the HUD anchored to the edges of the tall frame; compared by CONTENT, never by present number):
  tools/fill_compare.py runcheck RUN.OUT h3 WxH | h3off WxH | h3169
      the H3 exit-report lines of runtime/aspect.c.  h3: dk passes > 0, anchored passes > 0, objects anchored at the top > 0 and at the
      bottom > 0, band mismatch 0, test aid off 0, passes begun inside another pass 0, next objects outside a pass 0, objects of a class
      whose group disagrees 0, no `h3-band-mismatch` WARNING.  h3off (KHNMC_ASPECT_TEST_OFF=h3): every pass "test aid off", no object
      anchored.  h3169 (fill on a 16:9 screen): every pass "o = 0, nothing written", no object anchored.  Task 42 review: every role
      needs the line "H3 setScreenMode (f_126500) inside an object of an anchored pass": h3: 0 such calls in ANCHORED objects (centre
      ones, the talk windows' zoom, are counted and allowed), h3off / h3169: 0 calls; no `h3-inner-mode-anchored` WARNING; and the
      objects centred for want of a classification (a class not in the table, a generic child of a group not in the table) are printed
      as a NOTE with their (class, group) pairs (not a condition: they stay centred by design).
  tools/fill_compare.py h3pictures TALL_FRAMES OFF_FRAMES WxH [--min-pairs N] [--png DIR] [--elements LIST]
      the dumps (KHNMC_GFX_DUMP) of a run with H3 (TALL) and of its control with KHNMC_ASPECT_TEST_OFF=h3 (OFF), the same route, screen
      and clock.  A TALL dump and an OFF dump are the same moment when their SCENE region (GS x 92..356, y 75..295: the middle of the
      picture, where no anchored element is drawn at either place) is byte-identical, not black and not mostly white (a flash: drifted
      runs show the same flash at different moments), whatever their present numbers.
      The elements (GS boxes from the measurement M, runtime/aspect.c; pixels by the exe's narrow-screen MagicScale: x * 3W/2048 + W/8,
      y * 9W/16/416 + o): top: the INFORMATION bar (GS x -86..300, y 14..58; the D3D12 scissor of its text moves with it), the enemy
      gauge (428..590, 26..44); bottom: the command menu (-80..90, 282..406), the player gauges and face (357..597, 296..410); centre:
      the style rank (384..597, 130..280), a talk window (-47..507, 12..275), and everything else.  Per pair and per anchored element
      present (at least 200 pixels of its box differ between OFF and TALL, i.e. the element is drawn there in OFF and not in TALL):
      (e) its edge: the vertical shift at which most of those pixels reappear in TALL (exactly, all three channels) among the expected
      one (-o top, +o bottom; floor and ceil when o is not whole) and +-1, 2, 4, 8 rows from it, 0 and the opposite edge is the
      expected one, with at least 40 %% reappearing (the face's lower part is translucent: about 72 %% reappear on lavapipe) and 3
      points more than at any other (the INFORMATION bar fading in: 0.76 against 0.69): GS y 0 at frame row 0 / GS y 416 at the last
      row; (c) not cut: the moved box lies inside the frame, and for the one scissored element, the INFORMATION bar, at least 90 %%
      of its WHITE pixels inside the bar (every channel >= 200 in OFF where TALL's own pixel there is not white, i.e. not a flash
      behind it; not in its 4 border rows: its text on the dark bar) reappear exactly (a scissor left at the band would
      cut the text above the band's first row); judged in pairs where it has at least 200 white pixels, at least 3 such pairs.  Per pair:
      (r) outside the judged elements' boxes at both places (widened: GS x -86..310 / y 0..75, 380..620 / 10..60, -86..120 / 230..650,
      330..620 / 240..650; only those of LIST with --elements) at most 0.01 %% of the pixels differ: the centred HUD (lock-on, damage numbers, style rank, talk and menu
      windows) and the 3D are unchanged.  Exit 0 = PASS: at least N pairs (default 20), every anchored element of LIST (default all
      four) present in at least 3 pairs, (e) and (c) at every pair where it is present, (r) at every pair; 1 = FAIL; 2 = usage;
      3 = UNDECIDED (fewer than N shared moments and nothing failing in them).
  tools/fill_compare.py h3static TALL_FRAMES OFF_FRAMES WxH [--elements LIST] [--presents FROM-TO]
      the same runs WITHOUT shared moments (drifted GPU runs): per anchored element its footprint (the pixels OFF shows in at least
      half of its sampled dumps and TALL does not show there) reappears in TALL exactly o rows higher / lower, the strict best of the
      shifts tried; (c) the INFORMATION bar's text (its white pixels that are not the bar's static white, per dump where the bar is
      drawn): TALL's median at least 0.8 of OFF's (a scissor left at the band cuts it); (r) the centred style rank (GS x 440..600, y
      130..290) is found at shift 0, not at -o / +o.  Default elements: the four of h3pictures and the style rank.  LIST may also
      name the camp route's elements (mission counter, radar, EXP and munny: top; reaction command: bottom; camp menu: centre), each
      judged over the dumps of --presents FROM-TO (its window of the route).  Exit 0 = PASS, 1 = FAIL (a test not judged is a FAIL).
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
OFF_NAMES = {'h2': ('H2b', 'H2c'), 'h2b': ('H2b',), 'h2c': ('H2c',), 'h3': ('H3',), 'h4': ('H4',), 'h5': ('H5',)}


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
    want_aid = ''.join('%s off; ' % k for k in ('H2b', 'H2c', 'H3', 'H4', 'H5') if k in off) + ('H2 divides m11 by 2047/208 instead of r (the no-cull reference); ' if role == 'nocull' else '')
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


# ---- task 42: H3 ------------------------------------------------------------------------------------------------------------------
H3_RE = re.compile(r'^kh: aspect report: H3 \(HUD anchoring.*?\): passes (\d+): o = 0 \(16:9 or wider\), nothing written (\d+); the exe.s bars on \(no 3D view\), left centred (\d+); '
                   r'band mismatch \(h3-band-mismatch\), left (\d+); test aid off (\d+); anchored (\d+) \(empty list (\d+)\); begun inside another pass (\d+); '
                   r'objects: first \(H3a\) top (\d+) bottom (\d+) centre (\d+), next \(H3b\) top (\d+) bottom (\d+) centre (\d+); ends of the list \(rax = 0\) (\d+); next object outside a pass (\d+)$')
H3W_RE = re.compile(r'^kh: aspect report: H3 objects by the reason of their anchor:.*; class and group disagree (\d+)$')
# task 42 review finding 2: setScreenMode (f_126500) from inside an object of an anchored pass (f_126500 recomputes MagicScale)
H3I_RE = re.compile(r'^kh: aspect report: H3 setScreenMode \(f_126500\) inside an object of an anchored pass \(.*?\): (\d+): the loop.s MagicScale back after it (\d+); '
                    r'in anchored objects (\d+) \(the loop.s MagicScale back: the anchored MS\.w written again (\d+); another mode, left as the exe made it (\d+)\)$')
# task 42 review finding 4: the objects centred for want of a classification (listed, not a condition: they stay centred by design)
H3U_RE = re.compile(r'^kh: aspect report: H3 objects by the reason of their anchor:.*; class not in the table, centred \(its group is in the table\) (\d+); '
                    r'class not in the table, centred \(its group is not in the table either\) (\d+); generic child of a group not in the table (\d+); class and group disagree \d+$')


def h3check(run_out, role):
    """runcheck roles h3 / h3off / h3169 (task 42) -> (ok, lines)"""
    try:
        text = open(run_out, 'rb').read().decode('utf-8', 'replace').splitlines()
    except OSError as e:
        return False, ['  run.out %s: cannot be read (%s): FAIL' % (run_out, e)]
    lines, ok = [], True
    def say(good, what):
        nonlocal ok
        ok = ok and good
        lines.append('  %s: %s' % (what, 'PASS' if good else 'FAIL'))
    m = [x for x in (H3_RE.match(l) for l in text) if x]
    if not m:
        say(False, '%s: no H3 report line (a build without task 42, or KHNMC_ASPECT is not fill)' % role); return ok, lines
    v = [int(m[-1].group(i)) for i in range(1, 17)]
    passes, ident, bars, mism, off, anch, empty, nested, ft, fb, fcen, nt, nb, ncen, ends, nopass = v
    top, bot = ft + nt, fb + nb
    wm = [x for x in (H3W_RE.match(l) for l in text) if x]
    dis = int(wm[-1].group(1)) if wm else -1
    desc = ('%s: H3: passes %d, o = 0 %d, bars on %d, band mismatch %d, test aid off %d, anchored %d; objects top %d, bottom %d, centre %d; nested %d, '
            'outside a pass %d; class and group disagree %s' % (role, passes, ident, bars, mism, off, anch, top, bot, fcen + ncen, nested, nopass, dis if dis >= 0 else '(no line)'))
    if role == 'h3':
        say(passes > 0 and anch > 0 and top > 0 and bot > 0 and mism == 0 and off == 0 and nested == 0 and nopass == 0 and dis == 0 and ident == 0,
            desc + ' (anchored passes with objects at both edges; no mismatch, test aid, nesting or disagreement; no pass at o = 0)')
    elif role == 'h3off':
        say(passes > 0 and off == passes and anch == 0 and top + bot + fcen + ncen == 0, desc + ' (the test aid passes every pass through)')
    else:
        say(passes > 0 and ident == passes and anch == 0 and top + bot + fcen + ncen == 0, desc + ' (16:9: every pass o = 0, nothing written)')
    warn = [l for l in text if 'h3-band-mismatch' in l and 'WARNING' in l]
    say(not warn, '%s: h3-band-mismatch WARNING lines: %d' % (role, len(warn)))
    im = [x for x in (H3I_RE.match(l) for l in text) if x]
    if not im:
        say(False, '%s: no "H3 setScreenMode (f_126500) inside an object" line (a build without the task 42 review fix)' % role)
    else:
        ni, nback, nanch, nre, noth = (int(im[-1].group(i)) for i in range(1, 6))
        if role == 'h3':
            say(nanch == 0, '%s: setScreenMode inside an object of an anchored pass %d (the loop\'s MagicScale back after it %d), in ANCHORED objects %d (written '
                            'again %d, another mode %d): 0 required (h3-inner-mode-anchored: such an object draws part of itself in a mode of its own, not anchored; '
                            'only centre classes were measured doing it)' % (role, ni, nback, nanch, nre, noth))
        else:
            say(ni == 0, '%s: setScreenMode inside an object of an anchored pass %d (no pass is anchored in this role)' % (role, ni))
    warn = [l for l in text if 'h3-inner-mode-anchored' in l and 'WARNING' in l]
    say(not warn, '%s: h3-inner-mode-anchored WARNING lines: %d' % (role, len(warn)))
    um = [x for x in (H3U_RE.match(l) for l in text) if x]
    if um:
        ul, uc, gu = (int(um[-1].group(i)) for i in range(1, 4))
        cl = [l.split('H3 centred: ', 1)[1] for l in text if l.startswith('kh: aspect report: H3 centred: ')]
        lines.append('  %s: NOTE (not a condition: they stay centred by design): objects centred for want of a classification: of a class not in the table %d '
                     '(its group in the table %d, not %d), generic children of a group not in the table %d%s' % (role, ul + uc, ul, uc, gu, (': ' + '; '.join(cl)) if cl else ''))
    return ok, lines


# the element boxes in GS units (x0, x1, y0, y1) from the measurement M (M40/*-M-review.txt, run-B42/M/*): anchor -1 top, +1 bottom
# the INFORMATION bar is the one scissored HUD element (M: the only partial scissors are its f_126fd0 rects from 0x140472bd0 and the
# radar's 256x256 target): its text is the (c) test; the others can only be cut by the frame edge (the geometric test)
H3_ELEMENTS = [('INFORMATION bar', -1, (-86, 300, 14, 58)), ('enemy gauge', -1, (428, 590, 26, 44)),
               ('command menu', 1, (-80, 90, 282, 406)), ('player gauges', 1, (357, 597, 296, 410))]
H3_SCISSORED = ('INFORMATION bar',)
H3_WIDE = {'INFORMATION bar': (-1, (-86, 310, 0, 75)), 'enemy gauge': (-1, (380, 620, 10, 60)), 'command menu': (1, (-86, 120, 230, 650)),
           'player gauges': (1, (330, 620, 240, 650))}                                # (r) leaves out these boxes of the judged elements
H3_SCENE = (92, 356, 75, 295)
# task 42 review finding 3: h3static's centred elements (drawn at the same place with and without H3): the mod's style rank at its
# resting place (M: GS y mid-points 192..224, x 384..683 with its zoom; on lavapipe at rows 580..780 of 1920x1350, x 1600..1820)
H3_CENTRED = [('style rank', (440, 600, 130, 290))]
# task 42 review finding 1: the camp route's elements (input/route-camp.txt; run-B42/M/F-camp-fill1350-M.txt), judged by h3static with
# --elements and --presents (each is drawn in a window of the route only): top: the mission counter (dk::COUNTER, group 8: its children
# GS y -13.7..96.4, frames 9515-14004; the two counters "GUARD" / "REVERSAL" on lavapipe at GS x -75..200, y 15..60), the radar (dk::Radar: its scissor GS x 485..581, y 16..112, frames 20846+), the EXP and munny
# lines (dk::NEXT_EXP / GET_MUNNY, group 7: x -61..-10, y 37..92, frames 19153-19425); bottom: the reaction command
# (dk::ReactionDecideSprite, group 2: x -122..26, y 204.5..336.25, 25-frame windows); centre: the camp menu (Tz::, menu-id 8,
# frames 21575-21817; Tz::MenuItem x -42..427, y 71..313)
H3_CAMP = [('mission counter', -1, (-75, 200, 15, 60)), ('radar', -1, (485, 581, 16, 112)), ('EXP and munny', -1, (-61, -10, 37, 92)),
           ('reaction command', 1, (-122, 26, 205, 336))]
H3_CAMP_CENTRED = [('camp menu', (-42, 427, 71, 313))]


def h3_geom(w, h):
    """the exe's narrow-screen MagicScale (f_1433a0): x * 3W/2048 + W/8, y * 9W/16/416 + o"""
    msx, msz = 3.0 * w / 2048.0, w / 8.0
    msy = 9.0 * w / 16.0 / 416.0
    o = (h - 9.0 * w / 16.0) / 2.0
    def px(box, shift=0.0):
        x0, x1, y0, y1 = box
        rd = lambda v: int(np.floor(v + 0.5))
        return (max(0, rd(x0 * msx + msz)), min(w, rd(x1 * msx + msz)), rd(y0 * msy + o + shift), rd(y1 * msy + o + shift))
    return o, px


def h3_pair(a, b, o, px, elements, lines, tag):
    """one (TALL a, OFF b) pair: per element (present, e_ok, c_ok, detail); (r_ok, rest)"""
    h, w = a.shape[:2]
    res = {}
    for name, s, box in elements:
        x0, x1, y0, y1 = px(box)
        y0c, y1c = max(0, y0), min(h, y1)
        ob = b[y0c:y1c, x0:x1]
        E = (ob != a[y0c:y1c, x0:x1]).any(axis=2)
        ne = int(E.sum())
        if ne < 200:
            res[name] = (False, True, True, 'not drawn here (%d pixels differ)' % ne, False); continue
        so = s * o
        exp = sorted({int(np.floor(so)), int(np.ceil(so))})
        cand = set(exp) | {0, -int(round(so))} | {exp[0] + d for d in (-1, -2, -4, -8)} | {exp[-1] + d for d in (1, 2, 4, 8)}
        score, best_m = {}, {}
        for t in sorted(cand):
            if y0c + t < 0 or y1c + t > h:
                score[t] = -1.0; continue
            m = (a[y0c + t:y1c + t, x0:x1] == ob).all(axis=2) & E
            score[t] = m.sum() / float(ne); best_m[t] = m
        te = max(exp, key=lambda t: score[t])
        others = max(v for t, v in score.items() if t not in exp)
        e_ok = score[te] >= 0.4 and score[te] - others >= 0.03
        inside = y0 + te >= 0 and y1 + te <= h
        # (c): the moved box inside the frame, and the element's white pixels (text: every channel >= 200 in OFF) reappear exactly
        # (a scissor left at the band would cut the INFORMATION text above the band's first row; translucent parts are never white)
        sc = name in H3_SCISSORED
        er = np.nonzero(E.sum(axis=1) >= 4)[0]
        wht = E & (ob >= 200).all(axis=2) & ~(a[y0c:y1c, x0:x1] >= 200).all(axis=2)   # white in OFF, not white in TALL there: the
        if len(er):                                                                    # element's own white, not a flash behind it                                                                    # the text lies inside the bar: not its 4 border rows
            wht[:er[0] + 4] = False; wht[max(0, er[-1] - 3):] = False                   # (a flash seen through its antialiased edge is white)
        nw = int(wht.sum())
        wr = float((best_m[te] & wht).sum()) / nw if te in best_m and nw else 0.0
        judged = sc and nw >= 200
        c_ok = inside and (not judged or wr >= 0.9)
        ext = ('element rows %d..%d -> %d..%d%s' % (y0c + er[0], y0c + er[-1], y0c + er[0] + te, y0c + er[-1] + te,
               ('; text (white) pixels %d, %s' % (nw, ('%.3f reappear%s' % (wr, '' if wr >= 0.9 else ' [CUT]')) if judged else 'too few to judge')) if sc else '')) if len(er) else '-'
        det = ('%d px; %.2f reappear at %+d (expected %s), best other %.2f%s; %s%s' % (ne, score[te], te, '/'.join('%+d' % t for t in exp), others,
               '' if e_ok else ' [EDGE WRONG: best %+d]' % max(score, key=score.get), ext, '' if inside else ' [moved box outside the frame]'))
        res[name] = (True, e_ok, c_ok, det, judged)
    mask = np.ones((h, w), bool)
    for name in [e[0] for e in elements]:
        s, box = H3_WIDE[name]
        for sh in (0.0, s * o):
            x0, x1, y0, y1 = px(box, sh)
            mask[max(0, y0):max(0, min(h, y1)), x0:x1] = False
    rest = int(((a != b).any(axis=2) & mask).sum())
    r_ok = rest <= 0.0001 * mask.sum()
    lines.append('  %s: %s; rest: %d pixels differ%s' % (tag, '; '.join('%s: %s%s' % (k, v[3], '' if not v[0] else ' (e) %s (c) %s' % ('ok' if v[1] else 'FAIL', 'ok' if v[2] else 'FAIL'))
                                                            for k, v in res.items()), rest, '' if r_ok else ' [(r) FAIL]'))
    return res, r_ok, rest


def read_dumps(d):
    """index.tsv of KHNMC_GFX_DUMP, every row by present (read_index keys by game_frame and keeps one dump per game frame)"""
    rows = {}
    with open(os.path.join(d, 'index.tsv')) as f:
        head = f.readline().rstrip('\n').split('\t')
        for need in ('present', 'file'):
            if need not in head:
                raise SystemExit('fill_compare: %s/index.tsv: no column %s' % (d, need))
        for ln in f:
            c = dict(zip(head, ln.rstrip('\n').split('\t')))
            if c.get('present'):
                rows[int(c['present'])] = {'present': int(c['present']), 'file': c['file']}
    return rows


def h3pictures(tall_dir, off_dir, screen, min_pairs=20, png=None, only=None):
    """-> (exit code, lines): 0 PASS, 1 FAIL, 3 UNDECIDED (fewer than min_pairs shared moments and nothing failing in them)"""
    w, h = screen
    o, px = h3_geom(w, h)
    lines = []
    ti = read_dumps(tall_dir); oi = read_dumps(off_dir)
    els = [e for e in H3_ELEMENTS if not only or e[0] in only]
    sx0, sx1, sy0, sy1 = px(H3_SCENE)
    def key(arr):
        reg = arr[sy0:sy1, sx0:sx1]
        if not reg.any() or (reg >= 250).all(axis=2).mean() > 0.5:                      # black, or a white flash: says nothing about the moment
            return None
        return hash(reg.tobytes())
    offs = {}
    for r in sorted(oi.values(), key=lambda r: r['present']):
        b = read_ppm(os.path.join(off_dir, r['file']))
        if b.shape[:2] != (h, w):
            return 2, ['  %s is %dx%d, not %dx%d' % (r['file'], b.shape[1], b.shape[0], w, h)]
        k = key(b)
        if k is not None and k not in offs:
            offs[k] = (r['present'], os.path.join(off_dir, r['file']))
    seen = {e[0]: 0 for e in els}; bad = {e[0]: 0 for e in els}; textj = {e[0]: 0 for e in els}; rbad = 0; npairs = 0; firstbad = []
    for r in sorted(ti.values(), key=lambda r: r['present']):
        a = read_ppm(os.path.join(tall_dir, r['file']))
        if a.shape[:2] != (h, w):
            return 2, ['  %s is %dx%d, not %dx%d' % (r['file'], a.shape[1], a.shape[0], w, h)]
        k = key(a)
        if k is None or k not in offs:
            continue
        q, bp = offs[k]; b = read_ppm(bp); npairs += 1
        sub = []
        res, r_ok, rest = h3_pair(a, b, o, px, els, sub, 'TALL present %d / OFF present %d' % (r['present'], q))
        pair_bad = not r_ok
        rbad += 0 if r_ok else 1
        for name, v in res.items():
            if v[0]:
                seen[name] += 1; textj[name] += 1 if v[4] else 0
                if not (v[1] and v[2]):
                    bad[name] += 1; pair_bad = True
        if pair_bad and len(firstbad) < 6:
            firstbad += sub
        if png and (npairs <= 2 or pair_bad):
            os.makedirs(png, exist_ok=True)
            save_png(os.path.join(png, 'h3-tall-p%d.png' % r['present']), a); save_png(os.path.join(png, 'h3-off-p%d.png' % q), b)
        if npairs % 25 == 1:
            lines += sub
    ok = npairs >= min_pairs and rbad == 0
    anybad = rbad > 0
    lines.append('  pairs (the same moment by the scene region GS x %d..%d, y %d..%d): %d (at least %d)%s' % (H3_SCENE + (npairs, min_pairs, '' if npairs >= min_pairs else ': too few')))
    for e in els:
        name = e[0]; sc = name in H3_SCISSORED
        good = seen[name] >= 3 and bad[name] == 0 and (not sc or textj[name] >= 3); ok = ok and good; anybad = anybad or bad[name] > 0
        lines.append('  %-16s (%s): present in %d pairs%s, (e) or (c) failing in %d: %s' % (name, 'top' if e[1] < 0 else 'bottom', seen[name],
                     ', its text judged in %d (at least 3)' % textj[name] if sc else '', bad[name], 'PASS' if good else 'FAIL'))
    lines.append('  (r) the rest unchanged (lock-on, damage numbers, style rank, windows, 3D): failing in %d pairs: %s' % (rbad, 'PASS' if rbad == 0 else 'FAIL'))
    if firstbad:
        lines.append('  the first failing pairs:'); lines += firstbad
    if npairs < min_pairs and not anybad:
        lines.append('  => UNDECIDED: the two runs share only %d moments (GPU runs drift apart: fence timing); nothing failed in them. '
                     'h3static judges the edges without shared moments' % npairs)
        return 3, lines
    return (0 if ok else 1), lines


def h3static(tall_dir, off_dir, screen, only=None, samples=40, presents=None):
    """the edges WITHOUT shared moments (GPU runs drift apart): a HUD element's opaque pixels are the same at every moment while the
    scene behind it changes.  Per element: R = its box in OFF's middle sampled dump; its footprint F = the pixels of R that OFF shows
    in at least half of its sampled dumps (the element, not the scene) and TALL shows at that place in at most a fifth of its (TALL
    does not draw it there); score(t) = over TALL's sampled dumps, the mean share of F that TALL shows exactly at t rows away.  PASS:
    the best t among the expected shift (-o top / +o bottom; floor and ceil when o is not whole), +-1, 2, 4, 8 rows from it, 0 and the
    opposite edge is the expected one, score >= 0.5 and 0.1 above any other; F at least 500 pixels (at 1920x1350; scaled by the area).
    Task 42 review finding 3 (what h3pictures judges at shared moments, here without them):
    (c) the scissored element's TEXT (the INFORMATION bar): its white pixels (every channel >= 200) inside the box that are not the
        bar's static white (F's white pixels: the title), counted per sampled dump where the bar is drawn (at least half of F shown at
        that shift) in TALL at the expected shift and in OFF at its place: TALL's median at least 0.8 of OFF's (the scrolling text is
        the same text in both runs, at other moments: their medians agree within a few %%; a scissor left at the band cuts the text
        above the band's first row, about 9 tenths of it); judged when OFF's median is at least 200 white pixels (scaled by the area)
        in at least 3 dumps of each run, otherwise FAIL (not judged).
    (r) the CENTRED elements (H3_CENTRED: the style rank; with --elements also the camp menu) stay where the exe draws them: per element
        the reference is the OFF dump whose box holds the most pixels (largest channel > 40) that OFF shows in at least a quarter of its
        sampled dumps (it is drawn in a part of them only, the style rank in battle), its footprint those pixels; score(t) as above over
        t = 0, -o, +o (and +-1, 2, 8): PASS when score(0) >= 0.2 and score(0) is 0.1 above score(-o) and score(+o); F at least 500
        pixels, otherwise FAIL (not judged).  A background that stands still behind the element counts for the place it is at: the
        element needs a moving scene behind it (the battle) to be judged by this test.
    presents (FROM, TO): only the dumps of those presents (an element drawn in a window of the route).  -> (exit code, lines)"""
    w, h = screen
    o, px = h3_geom(w, h)
    els = [e for e in H3_ELEMENTS + H3_CAMP if (e[0] in only if only else e in H3_ELEMENTS)]
    cens = [e for e in H3_CENTRED + H3_CAMP_CENTRED if (e[0] in only if only else e in H3_CENTRED)]
    def sample(d):
        rs = sorted(read_dumps(d).values(), key=lambda r: r['present'])
        if presents:
            rs = [r for r in rs if presents[0] <= r['present'] <= presents[1]]
        if not rs:
            return []
        idx = sorted(set(np.linspace(0, len(rs) - 1, min(len(rs), samples)).astype(int)))
        out = []
        for i in idx:
            a = read_ppm(os.path.join(d, rs[i]['file']))
            if a.shape[:2] != (h, w):
                raise SystemExit('fill_compare: h3static: %s is %dx%d, not %dx%d' % (rs[i]['file'], a.shape[1], a.shape[0], w, h))
            out.append(a)
        return out
    TA, OA = sample(tall_dir), sample(off_dir)
    lines = ['  sampled dumps: TALL %d, OFF %d' % (len(TA), len(OA))]
    if len(TA) < 8 or len(OA) < 8:
        return 1, lines + ['  too few dumps (at least 8 per run): FAIL']
    ref = OA[len(OA) // 2]; ok = True
    minf = max(50, int(500.0 * w * h / (1920 * 1350)))                                  # 500 pixels at 1920x1350
    for name, s_, box in els:
        x0, x1, y0, y1 = px(box)
        R = ref[y0:y1, x0:x1]
        soff = np.mean([(b[y0:y1, x0:x1] == R).all(axis=2) for b in OA], axis=0)
        t0 = np.mean([(a[y0:y1, x0:x1] == R).all(axis=2) for a in TA], axis=0)
        F = (soff >= 0.5) & (t0 <= 0.2); nf = int(F.sum())
        if nf < minf:
            ok = False
            lines.append('  %-16s: footprint %d pixels (at least %d): FAIL (OFF does not draw it in half of its dumps, or TALL draws it at the old place)' % (name, nf, minf)); continue
        so = s_ * o
        exp = sorted({int(np.floor(so)), int(np.ceil(so))})
        cand = set(exp) | {0, -int(round(so))} | {exp[0] + d for d in (-1, -2, -4, -8)} | {exp[-1] + d for d in (1, 2, 4, 8)}
        sc = {}
        for t in sorted(cand):
            if y0 + t < 0 or y1 + t > h:
                continue
            sc[t] = float(np.mean([((a[y0 + t:y1 + t, x0:x1] == R).all(axis=2) & F).sum() / float(nf) for a in TA]))
        te = max(exp, key=lambda t: sc.get(t, -1.0)); other = max(v for t, v in sc.items() if t not in exp)
        good = te in sc and sc[te] >= 0.5 and sc[te] - other >= 0.1; ok = ok and good
        lines.append('  %-16s (%s): footprint %d pixels; %.3f at %+d (expected %s), best other %.3f (%+d), at 0 %.3f: %s' % (
            name, 'top' if s_ < 0 else 'bottom', nf, sc.get(te, -1), te, '/'.join('%+d' % t for t in exp), other,
            max((t for t in sc if t not in exp), key=lambda t: sc[t]), sc.get(0, -1), 'PASS' if good else 'FAIL'))
        if name in H3_SCISSORED:                                                        # (c): its text not cut (review finding 3)
            def whites(arrs, t):                                                        # the white masks of the dumps where the bar is drawn
                v = []
                for a in arrs:
                    if y0 + t < 0 or y1 + t > h:
                        continue
                    sub = a[y0 + t:y1 + t, x0:x1]
                    if ((sub == R).all(axis=2) & F).sum() >= 0.5 * nf:
                        v.append((sub >= 200).all(axis=2))
                return v
            wo, wt = whites(OA, 0), whites(TA, te)
            stw = (np.mean(wo, axis=0) >= 0.9) if wo else np.zeros(F.shape, bool)       # the bar's static white (its title): white in 9 tenths of OFF's
            vo, vt = [int((m & ~stw).sum()) for m in wo], [int((m & ~stw).sum()) for m in wt]
            mt, mo = (float(np.median(vt)) if vt else 0.0), (float(np.median(vo)) if vo else 0.0)
            minw = max(20, int(200.0 * w * h / (1920 * 1350)))
            judged = len(vt) >= 3 and len(vo) >= 3 and mo >= minw
            tg = judged and mt >= 0.8 * mo; ok = ok and tg
            lines.append('  %-16s (c) its text: white pixels (not its static white) per dump where it is drawn, median TALL %.0f (%d dumps) against OFF %.0f '
                         '(%d dumps): %s%s' % (name, mt, len(vt), mo, len(vo), ('%.3f of OFF\'s (at least 0.8) ' % (mt / mo)) if mo else '',
                                               'PASS' if tg else ('FAIL [TEXT CUT]' if judged else 'FAIL (not judged: OFF shows fewer than %d white pixels or fewer than 3 dumps)' % minw)))
    for name, box in cens:                                                              # (r): the centred elements (review finding 3)
        x0, x1, y0, y1 = px(box)
        y0, y1 = max(0, y0), min(h, y1)
        bo = [b[y0:y1, x0:x1] for b in OA]
        best = None
        for R in bo:
            soff = np.mean([(b == R).all(axis=2) for b in bo], axis=0)
            Fc = (soff >= 0.25) & (R.max(axis=2) > 40)
            nfc = int(Fc.sum())
            if best is None or nfc > best[0]:
                best = (nfc, R, Fc)
        nfc, R, Fc = best
        if nfc < minf:
            ok = False
            lines.append('  %-16s (centre): footprint %d pixels (at least %d): FAIL (not judged: not drawn in a quarter of OFF\'s dumps)' % (name, nfc, minf)); continue
        so = int(round(o))
        sc = {}
        for t in (0, -so, so, -1, 1, -2, 2, -8, 8):
            if y0 + t < 0 or y1 + t > h:
                continue
            sc[t] = float(np.mean([((a[y0 + t:y1 + t, x0:x1] == R).all(axis=2) & Fc).sum() / float(nfc) for a in TA]))
        edge = max(sc.get(-so, 0.0), sc.get(so, 0.0))
        good = sc.get(0, 0.0) >= 0.2 and sc.get(0, 0.0) - edge >= 0.1; ok = ok and good
        lines.append('  %-16s (centre): footprint %d pixels; %.3f at 0, %.3f at -%d, %.3f at +%d (+-1 %.3f / %.3f): %s' % (
            name, nfc, sc.get(0, -1), sc.get(-so, -1), so, sc.get(so, -1), so, sc.get(-1, -1), sc.get(1, -1), 'PASS' if good else 'FAIL [MOVED]'))
    return (0 if ok else 1), lines


def h3_synth(seed=42):
    """synthetic TALL / OFF pictures at 384x270 (o = 27): a textured scene, the four elements as opaque patterned blocks with a
    translucent lower half (the face), drawn centred in OFF and moved by -o / +o in TALL"""
    rng = np.random.default_rng(seed)
    w, h = 384, 270
    o, px = h3_geom(w, h)
    yy, xx = np.mgrid[0:h, 0:w]
    scene = np.zeros((h, w, 3), np.uint8)
    scene[..., 0] = (80 + 60 * np.sin(xx / 9.0) + 30 * np.cos(yy / 5.0)).clip(0, 255)
    scene[..., 1] = (100 + 50 * np.cos((xx + 2 * yy) / 7.0)).clip(0, 255)
    scene[..., 2] = rng.integers(40, 200, size=(h, w))
    def draw(img, box, shift, seed_, cut_text=0, phase=None):
        """cut_text = the element's first rows whose TEXT a scissor removes (the opaque bar itself stays, as with the real scissor);
        phase = the moment of a SCROLLING text (the INFORMATION message, task 42 review): its white pixels are the columns where
        (column + 3 phase) % 10 < 7, so no pixel is white at every moment; None: static white rows"""
        x0, x1, y0, y1 = px(box, shift)
        r = np.random.default_rng(seed_)
        pat = (r.integers(0, 180, size=(y1 - y0, x1 - x0, 3))).astype(np.uint8)       # the opaque bar: never white
        half = 2 * (y1 - y0) // 3
        for row in range(0, half, 3):
            if row >= cut_text:
                if phase is None:
                    pat[row] = 255                                                     # white "text" rows in the opaque part
                else:
                    pat[row, (np.arange(x1 - x0) + 3 * phase) % 10 < 7] = 255
        under = img[y0:y1, x0:x1].copy()
        img[y0:y0 + half, x0:x1] = pat[:half]
        lo = img[y0 + half:y1, x0:x1].astype(np.uint16)
        img[y0 + half:y1, x0:x1] = ((lo + (pat[half:] & 0x7f)) // 2).astype(np.uint8)  # translucent lower third
        img[y0:y1, x0:x1][:, 1::3] = under[:, 1::3]                                     # see-through columns (the face ring's box has holes)
    return w, h, o, px, scene, draw


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
    # task 42: runcheck h3 / h3off / h3169 and h3pictures
    def h3l(passes, ident, bars, mism, off, anch, ft, fb, fc_, nt, nb, nc, nested=0, nopass=0):
        return ('kh: aspect report: H3 (HUD anchoring, dk loop 0x166640: f_126500 at 0x140166677 = H3a, f_4ad270 at 0x140166693 = H3b): passes %d: o = 0 (16:9 or wider), nothing written %d; '
                'the exe\'s bars on (no 3D view), left centred %d; band mismatch (h3-band-mismatch), left %d; test aid off %d; anchored %d (empty list 0); begun inside another pass %d; '
                'objects: first (H3a) top %d bottom %d centre %d, next (H3b) top %d bottom %d centre %d; ends of the list (rax = 0) %d; next object outside a pass %d'
                % (passes, ident, bars, mism, off, anch, nested, ft, fb, fc_, nt, nb, nc, anch, nopass))
    def h3w(dis, ul=0, uc=0, gu=0):
        return ('kh: aspect report: H3 objects by the reason of their anchor: by its class 9; by its owner\'s group (+0x14) 9; class not in the table, centred (its group is in the table) %d; '
                'class not in the table, centred (its group is not in the table either) %d; generic child of a group not in the table %d; class and group disagree %d' % (ul, uc, gu, dis))
    def h3i(ni=0, back=0, anch=0, re_=0, oth=0):                                         # task 42 review finding 2
        return ('kh: aspect report: H3 setScreenMode (f_126500) inside an object of an anchored pass (any return address but 0x140166677 / 0x1401666bf; f_126500 recomputes MagicScale): %d: '
                'the loop\'s MagicScale back after it %d; in anchored objects %d (the loop\'s MagicScale back: the anchored MS.w written again %d; another mode, left as the exe made it %d)' % (ni, back, anch, re_, oth))
    h3_cases = [('runcheck h3: anchored at both edges', 'h3', [h3l(1000, 0, 400, 0, 0, 600, 0, 0, 600, 900, 1200, 300), h3w(0), h3i()], True),
                ('runcheck h3: a band mismatch', 'h3', [h3l(1000, 0, 400, 5, 0, 595, 0, 0, 595, 900, 1200, 300), h3w(0), h3i(), 'kh: aspect: WARNING: h3-band-mismatch: x'], False),
                ('runcheck h3: nothing at the top', 'h3', [h3l(1000, 0, 400, 0, 0, 600, 0, 0, 600, 0, 1200, 300), h3w(0), h3i()], False),
                ('runcheck h3: class and group disagree', 'h3', [h3l(1000, 0, 400, 0, 0, 600, 0, 0, 600, 900, 1200, 300), h3w(4), h3i()], False),
                ('runcheck h3: the test aid on', 'h3', [h3l(1000, 0, 0, 0, 1000, 0, 0, 0, 0, 0, 0, 0), h3w(0), h3i()], False),
                ('runcheck h3off: every pass passed through', 'h3off', [h3l(1000, 0, 0, 0, 1000, 0, 0, 0, 0, 0, 0, 0), h3w(0), h3i()], True),
                ('runcheck h3off: the test aid did not take effect', 'h3off', [h3l(1000, 0, 400, 0, 0, 600, 0, 0, 600, 900, 1200, 300), h3w(0), h3i()], False),
                ('runcheck h3169: o = 0 at every pass', 'h3169', [h3l(1000, 1000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0), h3w(0), h3i()], True),
                ('runcheck h3169: anchored at 16:9', 'h3169', [h3l(1000, 990, 0, 0, 0, 10, 0, 0, 10, 5, 5, 0), h3w(0), h3i()], False),
                ('runcheck h3: no H3 line (an older build)', 'h3', ['kh: aspect report: KHNMC_ASPECT=fill'], False),
                # task 42 review finding 2: setScreenMode inside objects of an anchored pass: in centre objects (the talk window's zoom) allowed,
                # in an anchored object a failure; the line itself required (a build before the fix has none)
                ('runcheck h3: setScreenMode inside centre objects (camp)', 'h3', [h3l(1000, 0, 400, 0, 0, 600, 0, 0, 600, 900, 1200, 300), h3w(0), h3i(346, 173)], True),
                ('runcheck h3: setScreenMode inside an anchored object', 'h3', [h3l(1000, 0, 400, 0, 0, 600, 0, 0, 600, 900, 1200, 300), h3w(0), h3i(348, 174, 2, 1, 1)], False),
                ('runcheck h3: an h3-inner-mode-anchored WARNING line', 'h3', [h3l(1000, 0, 400, 0, 0, 600, 0, 0, 600, 900, 1200, 300), h3w(0), h3i(),
                                                                                'kh: aspect: WARNING: h3-inner-mode-anchored: x'], False),
                ('runcheck h3: no inner setScreenMode line (before the fix)', 'h3', [h3l(1000, 0, 400, 0, 0, 600, 0, 0, 600, 900, 1200, 300), h3w(0)], False),
                ('runcheck h3off: inner setScreenMode counted', 'h3off', [h3l(1000, 0, 0, 0, 1000, 0, 0, 0, 0, 0, 0, 0), h3w(0), h3i(2, 1)], False)]
    # task 42 review finding 4: objects centred for want of a classification are listed as a NOTE (with their pairs), not a failure
    h3_note = ('runcheck h3: unlisted classes printed, still PASS', [h3l(1000, 0, 400, 0, 0, 600, 0, 0, 600, 900, 1200, 300), h3w(0, 3, 1, 2), h3i(),
                                                                       'kh: aspect report: H3 centred: class not in the table, centred (its group is in the table) vtable 0x1405b1234 .?AVX@dk@@, group (+0x14) 7: 3 objects'])
    with tempfile.TemporaryDirectory() as td:
        for k, (name, role, lines_, want) in enumerate(h3_cases):
            pth = os.path.join(td, 'h3_%02d.out' % k); open(pth, 'w').write('\n'.join(lines_) + '\n')
            ok, out = h3check(pth, role)
            good_ = ok == want; n += 1; fails += 0 if good_ else 1
            print('fill_compare selftest: %-58s %s (expected %s)' % (name, 'PASS' if ok else 'FAIL', 'PASS' if want else 'FAIL') + ('' if good_ else '   <-- WRONG\n' + '\n'.join(out)))
        pth = os.path.join(td, 'h3_note.out'); open(pth, 'w').write('\n'.join(h3_note[1]) + '\n')
        ok, out = h3check(pth, 'h3')
        good_ = ok and any('NOTE' in l and 'of a class not in the table 4 (its group in the table 3, not 1), generic children of a group not in the table 2: '
                           'class not in the table, centred (its group is in the table) vtable 0x1405b1234 .?AVX@dk@@, group (+0x14) 7: 3 objects' in l for l in out)
        n += 1; fails += 0 if good_ else 1
        print('fill_compare selftest: %-58s %s (expected PASS with the NOTE)' % (h3_note[0], 'PASS' if ok else 'FAIL') + ('' if good_ else '   <-- WRONG\n' + '\n'.join(out)))
        w3, h3_, o3, px3, scene0, draw = h3_synth()
        rank0 = lambda i: 0.0 if i % 2 == 0 else None                                            # the style rank: centred, in every other moment
        def frames(name, variant, nfr=4, roll=7, rank=rank0, extra=()):
            """variant(element, s, moment) -> (shift or None, text rows cut); rank(moment) -> the style rank's shift or None (review
            finding 3: h3static's centred element); extra: more (name, s, box) elements drawn by variant (the camp route's)"""
            d = os.path.join(td, name); os.makedirs(d)
            with open(os.path.join(d, 'index.tsv'), 'w') as f:
                f.write('present\tfile\twidth\theight\tdraws_total\tgame_frame\n')
                for i in range(nfr):
                    img = np.roll(scene0, roll * i, axis=1).copy()                                      # the moments (roll 0: a static scene)
                    for j, (en, s_, box) in enumerate(list(H3_ELEMENTS) + list(extra)):
                        sh, cut = variant(en, s_, i)
                        if sh is not None:
                            draw(img, box, sh, 100 + j, cut, i if en in H3_SCISSORED else None)    # the INFORMATION text scrolls
                    if rank(i) is not None:
                        draw(img, H3_CENTRED[0][1], rank(i), 200, 0)
                    fn = 'frame-%06d.ppm' % (100 + i)
                    with open(os.path.join(d, fn), 'wb') as g:
                        g.write(b'P6\n%d %d\n255\n' % (w3, h3_)); g.write(img.tobytes())
                    f.write('%d\t%s\t%d\t%d\t%d\t%d\n' % (100 + i, fn, w3, h3_, 1000 * i, 100 + i))
            return d
        off_d = frames('off', lambda en, s_, i: (0.0, 0))
        hp3 = [('h3pictures: the four elements at their edges', frames('t_ok', lambda en, s_, i: (s_ * o3, 0)), 0),
               ('h3pictures: H3 off in both runs (nothing moved)', frames('t_same', lambda en, s_, i: (0.0, 0)), 1),
               ('h3pictures: the top elements one row off', frames('t_off1', lambda en, s_, i: (s_ * o3 + (1 if s_ < 0 else 0), 0)), 1),
               ('h3pictures: the INFORMATION text cut by the band scissor', frames('t_cut', lambda en, s_, i: (s_ * o3, 8 if en == 'INFORMATION bar' else 0)), 1),
               ('h3pictures: the command menu moved to the top', frames('t_flip', lambda en, s_, i: ((-o3 if en == 'command menu' else s_ * o3), 0)), 1),
               ('h3pictures: an element moved by half the offset', frames('t_half', lambda en, s_, i: (s_ * o3 / 2 if en == 'player gauges' else s_ * o3, 0)), 1)]
        # an element never drawn in either run (here the enemy gauge) is a failure, unless --elements leaves it out
        off_ne = frames('off_ne', lambda en, s_, i: (None if en == 'enemy gauge' else 0.0, 0))
        t_ne = frames('t_ne', lambda en, s_, i: (None if en == 'enemy gauge' else s_ * o3, 0))
        code, out = h3pictures(t_ne, off_ne, (w3, h3_), min_pairs=3)
        good_ = code == 1 and any(l.startswith('  enemy gauge') and 'present in 0 pairs' in l and l.endswith('FAIL') for l in out); n += 1; fails += 0 if good_ else 1
        print('fill_compare selftest: %-58s %d (expected 1)' % ('h3pictures: the enemy gauge never drawn', code) + ('' if good_ else '   <-- WRONG\n' + '\n'.join(out)))
        code, out = h3pictures(t_ne, off_ne, (w3, h3_), min_pairs=3, only=['INFORMATION bar', 'command menu', 'player gauges'])
        good_ = code == 0; n += 1; fails += 0 if good_ else 1
        print('fill_compare selftest: %-58s %d (expected 0)' % ('h3pictures: the same, --elements without it', code) + ('' if good_ else '   <-- WRONG\n' + '\n'.join(out)))
        for name, d, want in hp3:
            code, out = h3pictures(d, off_d, (w3, h3_), min_pairs=3)
            good_ = code == want; n += 1; fails += 0 if good_ else 1
            print('fill_compare selftest: %-58s %d (expected %d)' % (name, code, want) + ('' if good_ else '   <-- WRONG\n' + '\n'.join(out)))
        # (r): a centred element (the style rank's box) drawn moved in TALL; and no common moment at all
        drest = frames('t_rest', lambda en, s_, i: (s_ * o3, 0))
        for i in range(4):
            fp = os.path.join(drest, 'frame-%06d.ppm' % (100 + i)); a = read_ppm(fp).copy()
            x0, x1, y0, y1 = px3((384, 597, 130, 280)); a[y0 + 10:y1 + 10, x0:x1] = a[y0:y1, x0:x1].copy()      # the style rank 10 rows lower
            with open(fp, 'wb') as g:
                g.write(b'P6\n%d %d\n255\n' % (w3, h3_)); g.write(a.tobytes())
        code, out = h3pictures(drest, off_d, (w3, h3_), min_pairs=3)
        good_ = code == 1 and any('(r) the rest unchanged' in l and l.endswith('FAIL') for l in out); n += 1; fails += 0 if good_ else 1
        print('fill_compare selftest: %-58s %d (expected 1, on (r))' % ('h3pictures: a centred element moved', code) + ('' if good_ else '   <-- WRONG\n' + '\n'.join(out)))
        dnone = frames('t_none', lambda en, s_, i: (s_ * o3, 0))
        for i in range(4):
            fp = os.path.join(dnone, 'frame-%06d.ppm' % (100 + i)); a = read_ppm(fp).copy(); a[..., 2] ^= 1
            with open(fp, 'wb') as g:
                g.write(b'P6\n%d %d\n255\n' % (w3, h3_)); g.write(a.tobytes())
        code, out = h3pictures(dnone, off_d, (w3, h3_), min_pairs=3)
        good_ = code == 3; n += 1; fails += 0 if good_ else 1
        print('fill_compare selftest: %-58s %d (expected 3 UNDECIDED)' % ('h3pictures: no common moment', code) + ('' if good_ else '   <-- WRONG\n' + '\n'.join(out)))
        # too few shared moments but an element failing in them: FAIL, not UNDECIDED
        code, out = h3pictures(frames('t_off1b', lambda en, s_, i: (s_ * o3 + (1 if s_ < 0 else 0), 0)), off_d, (w3, h3_), min_pairs=10)
        good_ = code == 1; n += 1; fails += 0 if good_ else 1
        print('fill_compare selftest: %-58s %d (expected 1)' % ('h3pictures: one row off, too few moments', code) + ('' if good_ else '   <-- WRONG\n' + '\n'.join(out)))
        # a white flash over the scene region at different moments of two drifted runs: not a shared moment (UNDECIDED, not a pair)
        def flash(name, variant, shift):
            d = frames(name, variant)
            for r in read_dumps(d).values():
                fp = os.path.join(d, r['file']); a = np.roll(read_ppm(fp), shift, axis=1).copy()
                sx0_, sx1_, sy0_, sy1_ = px3(H3_SCENE); a[sy0_ - 3:sy1_ + 3, sx0_ - 3:sx1_ + 3] = 255
                with open(fp, 'wb') as g:
                    g.write(b'P6\n%d %d\n255\n' % (w3, h3_)); g.write(a.tobytes())
            return d
        code, out = h3pictures(flash('t_flash', lambda en, s_, i: (s_ * o3, 0), 5), flash('o_flash', lambda en, s_, i: (0.0, 0), 0), (w3, h3_), min_pairs=3)
        good_ = code == 3; n += 1; fails += 0 if good_ else 1
        print('fill_compare selftest: %-58s %d (expected 3)' % ('h3pictures: a white flash at different moments', code) + ('' if good_ else '   <-- WRONG\n' + '\n'.join(out)))
        # h3static: the same synthetic runs, judged without shared moments (TALL's dumps rolled by 3 frames: no moment is shared)
        off10 = frames('off10', lambda en, s_, i: (0.0, 0), 10)
        def rolled(name, variant, **kw):
            d = frames(name, variant, 10, **kw); rs = sorted(read_dumps(d).values(), key=lambda r: r['present'])
            arrs = [read_ppm(os.path.join(d, r['file'])).copy() for r in rs]
            for k, r in enumerate(rs):
                with open(os.path.join(d, r['file']), 'wb') as g:
                    g.write(b'P6\n%d %d\n255\n' % (w3, h3_)); g.write(arrs[(k + 3) % len(arrs)].tobytes())
            return d
        hs = [('h3static: the four elements at their edges', rolled('s_ok', lambda en, s_, i: (s_ * o3, 0)), 0),
              ('h3static: H3 off in both runs (nothing moved)', rolled('s_same', lambda en, s_, i: (0.0, 0)), 1),
              ('h3static: the top elements one row off', rolled('s_off1', lambda en, s_, i: (s_ * o3 + (1 if s_ < 0 else 0), 0)), 1),
              ('h3static: the command menu moved to the top', rolled('s_flip', lambda en, s_, i: ((-o3 if en == 'command menu' else s_ * o3), 0)), 1),
              ('h3static: an element moved by half the offset', rolled('s_half', lambda en, s_, i: (s_ * o3 / 2 if en == 'player gauges' else s_ * o3, 0)), 1)]
        # an element drawn at BOTH places (moved, and a ghost left in the band): its old place is no footprint -> FAIL
        dboth = frames('s_both', lambda en, s_, i: (s_ * o3, 0), 10)
        for k, r in enumerate(sorted(read_dumps(dboth).values(), key=lambda r: r['present'])):
            fp = os.path.join(dboth, r['file']); a = read_ppm(fp).copy()
            for j, (en, s_, box) in enumerate(H3_ELEMENTS):
                draw(a, box, 0.0, 100 + j, 0)
            with open(fp, 'wb') as g:
                g.write(b'P6\n%d %d\n255\n' % (w3, h3_)); g.write(a.tobytes())
        hs.append(('h3static: every element drawn at both places', dboth, 1))
        off_st = frames('off_static', lambda en, s_, i: (0.0, 0), 10, 0)                      # a camera standing still: the scene behind
        hs_static = ('h3static: correct, over a static scene', frames('s_static', lambda en, s_, i: (s_ * o3, 0), 10, 0), 0)   # is no footprint
        for name, d, want in hs + [hs_static]:
            code, out = h3static(d, off_st if d == hs_static[1] else off10, (w3, h3_))
            good_ = code == want; n += 1; fails += 0 if good_ else 1
            print('fill_compare selftest: %-58s %d (expected %d)' % (name, code, want) + ('' if good_ else '   <-- WRONG\n' + '\n'.join(out)))
        # task 42 review finding 3: h3static's (c) and (r) without shared moments; each must fail on ITS line, every other line passing
        def only_fail(out, mark):
            fl = [l for l in out if 'FAIL' in l]
            return len(fl) == 1 and mark in fl[0]
        hs3 = [('h3static: the INFORMATION text cut by the band scissor', rolled('s_cut', lambda en, s_, i: (s_ * o3, 8 if en == 'INFORMATION bar' else 0)), None, '[TEXT CUT]'),
               ('h3static: the style rank moved to the top (-o)', rolled('s_rank', lambda en, s_, i: (s_ * o3, 0), rank=lambda i: -o3 if i % 2 == 0 else None), None, '[MOVED]'),
               ('h3static: the style rank moved down by 10 rows', rolled('s_rank10', lambda en, s_, i: (s_ * o3, 0), rank=lambda i: 10.0 if i % 2 == 0 else None), None, '[MOVED]'),
               ('h3static: the style rank missing in TALL', rolled('s_norank', lambda en, s_, i: (s_ * o3, 0), rank=lambda i: None), None, '[MOVED]'),
               ('h3static: the style rank in neither run (not judged)', rolled('s_norank2', lambda en, s_, i: (s_ * o3, 0), rank=lambda i: None), None, 'not judged')]
        off10n = frames('off10n', lambda en, s_, i: (0.0, 0), 10, rank=lambda i: None)
        for name, d, only_, mark in hs3:
            code, out = h3static(d, off10n if 'in neither' in name else off10, (w3, h3_), only_)
            good_ = code == 1 and only_fail(out, mark); n += 1; fails += 0 if good_ else 1
            print('fill_compare selftest: %-58s %d (expected 1, on %s only)' % (name, code, mark) + ('' if good_ else '   <-- WRONG\n' + '\n'.join(out)))
        code, out = h3static(hs3[4][1], off10n, (w3, h3_), [e[0] for e in H3_ELEMENTS])
        good_ = code == 0; n += 1; fails += 0 if good_ else 1
        print('fill_compare selftest: %-58s %d (expected 0)' % ('h3static: the same, --elements the four anchored ones', code) + ('' if good_ else '   <-- WRONG\n' + '\n'.join(out)))
        # task 42 review finding 1: a camp-route element (the radar, top) drawn in moments 0..7 of 20 only: judged over its window
        # (--presents), not over the whole run (there it is no footprint); drawn unmoved in TALL it fails
        radar = [e for e in H3_CAMP if e[0] == 'radar']
        cv = lambda sh: (lambda en, s_, i: ((sh(s_) if i < 8 else None) if en == 'radar' else None, 0))
        c_off = frames('c_off', cv(lambda s_: 0.0), 20, rank=lambda i: None, extra=radar)
        c_ok = frames('c_ok', cv(lambda s_: s_ * o3), 20, 11, rank=lambda i: None, extra=radar)
        c_same = frames('c_same', cv(lambda s_: 0.0), 20, 11, rank=lambda i: None, extra=radar)
        for name, d, pw, want in (('h3static: the radar (camp) over its window', c_ok, (100, 107), 0), ('h3static: the radar over the whole run (no footprint)', c_ok, None, 1),
                                  ('h3static: the radar unmoved, over its window', c_same, (100, 107), 1)):
            code, out = h3static(d, c_off, (w3, h3_), ['radar'], presents=pw)
            good_ = code == want; n += 1; fails += 0 if good_ else 1
            print('fill_compare selftest: %-58s %d (expected %d)' % (name, code, want) + ('' if good_ else '   <-- WRONG\n' + '\n'.join(out)))
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
    out = opt('--out'); png = opt('--png'); mp = opt('--min-pairs'); ms = opt('--min-share'); opt_els = opt('--elements'); opt_pres = opt('--presents')
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
                print('fill_compare: runcheck RUN.OUT off WxH LIST: LIST is the KHNMC_ASPECT_TEST_OFF value (h2, h2b, h2c, h3, h4, h5, comma-separated)', file=sys.stderr); return 2
        elif len(av) == 5:
            print(__doc__); return 2
        ok, out = fillcheck(av[1], av[2], sc, offl)
        print('fill_compare: runcheck %s (%s%s): %s' % (av[1], av[2], ' ' + ','.join(offl) if offl else '', 'PASS' if ok else 'FAIL')); print('\n'.join(out)); return 0 if ok else 1
    if av[0] == 'runcheck' and len(av) in (3, 4) and av[2] in ('h3', 'h3off', 'h3169'):
        ok, out = h3check(av[1], av[2])
        print('fill_compare: runcheck %s (%s): %s' % (av[1], av[2], 'PASS' if ok else 'FAIL')); print('\n'.join(out)); return 0 if ok else 1
    if av[0] == 'h3static' and len(av) == 4:
        m = re.fullmatch(r'(\d+)x(\d+)', av[3])
        if not m:
            print('fill_compare: h3static: the screen must be WxH, not %s' % av[3], file=sys.stderr); return 2
        els = opt_els.split(',') if opt_els else None
        known = [x[0] for x in H3_ELEMENTS + H3_CAMP + H3_CENTRED + H3_CAMP_CENTRED]
        if els and any(e not in known for e in els):
            print('fill_compare: h3static: --elements takes names of %s' % ', '.join(known), file=sys.stderr); return 2
        pw = None
        if opt_pres:
            mp_ = re.fullmatch(r'(\d+)-(\d+)', opt_pres)
            if not mp_ or int(mp_.group(1)) > int(mp_.group(2)):
                print('fill_compare: h3static: --presents takes FROM-TO (present numbers, FROM <= TO), not %s' % opt_pres, file=sys.stderr); return 2
            pw = (int(mp_.group(1)), int(mp_.group(2)))
        for d in av[1:3]:
            if not os.path.isfile(os.path.join(d, 'index.tsv')):
                print('fill_compare: %s/index.tsv is missing' % d, file=sys.stderr); return 2
        code, out = h3static(av[1], av[2], (int(m.group(1)), int(m.group(2))), els, presents=pw)
        print('fill_compare: h3static %s %s %s: %s' % (av[1], av[2], av[3], 'PASS' if code == 0 else 'FAIL')); print('\n'.join(out)); return code
    if av[0] == 'h3pictures' and len(av) == 4:
        m = re.fullmatch(r'(\d+)x(\d+)', av[3])
        if not m:
            print('fill_compare: h3pictures: the screen must be WxH, not %s' % av[3], file=sys.stderr); return 2
        els = opt_els.split(',') if opt_els else None
        if els and any(e not in [x[0] for x in H3_ELEMENTS] for e in els):
            print('fill_compare: h3pictures: --elements takes names of %s' % ', '.join(x[0] for x in H3_ELEMENTS), file=sys.stderr); return 2
        for d in av[1:3]:
            if not os.path.isfile(os.path.join(d, 'index.tsv')):
                print('fill_compare: %s/index.tsv is missing' % d, file=sys.stderr); return 2
        code, out = h3pictures(av[1], av[2], (int(m.group(1)), int(m.group(2))), int(mp) if mp else 20, png, els)
        print('fill_compare: h3pictures %s %s %s: %s' % (av[1], av[2], av[3], {0: 'PASS', 1: 'FAIL', 3: 'UNDECIDED'}[code])); print('\n'.join(out)); return code
    if av[0] == 'selftest' and len(av) == 1:
        return selftest()
    print(__doc__); return 2


if __name__ == '__main__':
    sys.exit(main())
