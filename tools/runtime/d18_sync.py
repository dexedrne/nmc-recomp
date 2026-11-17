#!/usr/bin/env python3
"""d18_sync.py - numeric picture / sound / game-event alignment of a KHNMC_CLOCK=frames run (TASK18).

  d18_sync.py --run RUN.out [--index VIDEO.tsv] --wav AUDIO.wav [--audiolog AUDIO.log] [--window-ms 40]

(without --index, e.g. a null-backend run, part 2 and the picture columns of part 3 are left out)

What it prints (and checks; exit 2 when a check fails):
 1. device clock: the WAV's length against the virtual clock.  Pass n of the mixer is due at t0 + n*10 ms of virtual time and the
    clock waits for it, so a run that ended at virtual time T holds floor((T - t0)/10 ms) + 1 passes of 480 sample frames (the last
    one may be missing when the run ends inside it).  t0 and the clock's start are read from RUN.out, T from the vblank count.
 2. picture cadence: picture k of the video index is virtual vblank v0 + k and WAV sample s0 + 800*k.
 3. game events: every `kh: event damage` line (BATTLE.calc_damage, written by the game thread with the virtual time of the frame it
    runs in) is placed in the picture stream (picture number, seconds into the video) and in the WAV (sample frame).
 4. sound against those events: the WAV's energy in 10 ms steps around each damage event, averaged over all events, against the
    same average around control instants (each event shifted by +/- 0.37 s, 0.61 s, 0.83 s).  A hit makes a sound, and so does the
    swing before it: if the sound is on the game's clock, the event average rises above the control average AT the events.
    Reported: the whole profile.  Checked (8 events or more): the largest ratio lies 0..150 ms after the event and is at least 1.25
    times the level 120..50 ms before it.  This is statistics over the whole mix, music included; part 5 is exact.
 5. with --audiolog (lines stamped [v=<virtual ns> vbl=<vblank>]): how many damage events have a source-voice Start call in their own
    or the next vblank, against the same count for control vblanks (the events moved by 22, 37, 50 vblanks either way), and where the
    first such voice lands in the WAV (the first pass after the call).  Checked (8 events or more): the events' share is above 1.5
    times the controls' share."""
import argparse, re, sys, wave
import numpy as np


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--run', required=True); ap.add_argument('--index'); ap.add_argument('--wav', required=True)
    ap.add_argument('--audiolog'); ap.add_argument('--window-ms', type=int, default=40)
    a = ap.parse_args()
    txt = open(a.run, errors='replace').read()
    fail = []
    m = re.search(r'pass 0 \(WAV sample 0\) is at virtual time (\d+) ns', txt)
    if not m: sys.exit('d18_sync: RUN.out has no "pass 0 (WAV sample 0) is at virtual time" line (was KHNMC_CLOCK=frames with KHNMC_AUDIO=wav: used?)')
    t0 = int(m.group(1))
    m = re.search(r'the virtual clock leaves the host clock at (\d+) ns', txt)
    if not m: sys.exit('d18_sync: RUN.out has no "virtual clock leaves the host clock" line')
    vt0 = int(m.group(1))
    m = re.search(r'kh: vclock: mode=frames .*virtual-vblanks=(\d+) released by: (.*?); frame-semaphore', txt)
    vbl_total = int(m.group(1)) if m else None
    with wave.open(a.wav, 'rb') as w:
        nch, width, rate, nfr = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        pcm = np.frombuffer(w.readframes(nfr), dtype='<i2').reshape(-1, nch).astype(np.float64)
    print(f'1. device clock: WAV {nfr} sample frames = {nfr / 48000:.3f} s, {nch} ch {8 * width} bit {rate} Hz; sample 0 = virtual time {t0} ns; the virtual clock started stepping at {vt0} ns ({(vt0 - t0) / 1e6:+.1f} ms from sample 0)')
    if vbl_total is not None:
        T = vt0 + vbl_total * 1000000000 // 60
        passes = (T - t0) // 10000000 + 1
        print(f'   run ended after {vbl_total} virtual vblanks = virtual time +{(T - vt0) / 1e9:.3f} s ({m.group(2)}); expected passes {passes} = {passes * 480} sample frames; WAV has {nfr // 480} passes ({nfr - passes * 480:+d} sample frames)')
        if not (-480 <= nfr - passes * 480 <= 0): fail.append(f'WAV length {nfr} is not the {passes * 480} sample frames of the virtual clock (one pass less is allowed)')
    else:
        print('   (no end-of-run vclock report in RUN.out: length not checked)')
    if a.index:
        rows = [tuple(int(x) for x in ln.split('\t')) for ln in open(a.index).read().splitlines()[1:]]
        v0, s0 = rows[0][1], rows[0][6]
        dev = max(abs((r[6] - s0) - 800 * k) for k, r in enumerate(rows)); gaps = sum(1 for k, r in enumerate(rows) if r[1] != v0 + k)
        exp_s0 = (rows[0][2] - t0) * 48000 // 1000000000
        print(f'2. picture cadence: {len(rows)} pictures, vblanks {v0}..{rows[-1][1]} ({gaps} out of sequence), picture 0 at WAV sample {s0} (from its virtual time: {exp_s0}), picture k at {s0} + 800*k within {dev} sample frame(s)')
        if gaps or dev > 1 or abs(exp_s0 - s0) > 1: fail.append('picture cadence')
    else:
        print('2. picture cadence: no video index given (pictures below are counted from the first virtual vblank)')
        rows = [(0, 0, vt0, 0, 0, 1, (vt0 - t0) * 48000 // 1000000000), (0, 10 ** 12, 0, 0, 0, 1, 0)]
        v0, s0 = rows[0][1], rows[0][6]
    ev = [(int(k), int(p), int(gf), int(vb), int(vn)) for k, p, gf, vb, vn in re.findall(r'kh: event damage #(\d+) present=(\d+) game-frame=(\d+) vblank=(\d+) virtual-ns=(\d+)', txt)]
    inwin = [e for e in ev if v0 <= e[3] <= rows[-1][1]]
    print(f'3. game events: {len(ev)} BATTLE.calc_damage calls in the run, {len(inwin)} inside the video window')
    for k, p, gf, vb, vn in inwin[:12]:
        pic = vb - v0; smp = (vn - t0) * 48000 // 1000000000
        print(f'   damage #{k}: game frame {gf}, present {p}, virtual vblank {vb} -> runs in the frame shown as picture {pic + 1} ({(pic + 1) / 60:.3f} s into the video); its instant = WAV sample {smp} ({smp / 48000:.3f} s) = video sound at {(smp - s0) / 48000:.3f} s')
    if len(inwin) > 12: print(f'   ... {len(inwin) - 12} more')
    # 4. event-triggered energy
    mono = pcm.mean(axis=1); hp = np.diff(mono, prepend=mono[:1])            # first difference: lifts transients over sustained music
    step = 480; nst = len(hp) // step; e = np.sqrt((hp[:nst * step].reshape(nst, step) ** 2).mean(axis=1))
    def profile(samples, lo=-12, hi=24):
        acc = np.zeros(hi - lo); n = 0
        for s in samples:
            i = int(s) // step
            if i + lo < 0 or i + hi > nst: continue
            acc += e[i + lo:i + hi]; n += 1
        return (acc / n if n else acc), n
    evs = [(vn - t0) * 48000 // 1000000000 for _, _, _, _, vn in ev]
    if evs:
        pe, ne = profile(evs)
        ctl = [s + int(d * 48000) for s in evs for d in (-0.83, -0.61, -0.37, 0.37, 0.61, 0.83)]
        pc, nc = profile(ctl)
        ratio = pe / np.maximum(pc, 1e-9)
        print(f'4. sound around the damage events: differentiated-signal RMS per 10 ms step, mean over {ne} events / mean over {nc} control instants')
        print('   step start (ms): ' + ' '.join(f'{10 * k:5d}' for k in range(-12, 24)))
        print('   event / control: ' + ' '.join(f'{r:5.2f}' for r in ratio))
        w = max(1, a.window_ms // 10); after = ratio[12:12 + w].mean(); before = ratio[:12].mean(); late = ratio[12 + 10:].mean(); pk = int(np.argmax(ratio)) - 12
        print(f'   mean ratio in the {10 * w} ms after the event {after:.2f}; in the 120 ms before {before:.2f}; 100..240 ms after {late:.2f}; largest ratio {ratio.max():.2f} in the step starting {10 * pk} ms after the event')
        early = ratio[:7].mean()                                                                # -120 .. -50 ms: before the swing that lands the hit
        print(f'   mean ratio 120..50 ms before the event: {early:.2f}; largest ratio / that level = {ratio.max() / early:.2f}')
        if ne >= 8 and not (0 <= pk <= 15 and ratio.max() >= 1.25 * early): fail.append(f'no sound rise at the damage events (level {early:.2f} at -120..-50 ms, largest ratio {ratio.max():.2f} at {10 * pk} ms)')
        if ne < 8: print('   (fewer than 8 events: reported, not checked)')
    else:
        print('4. no damage events in the run: nothing to compare')
    # 5. voice starts from the stamped audio log
    if a.audiolog:
        starts = [(int(v), int(vb), ln) for v, vb, ln in re.findall(r'^\[v=(\d+) vbl=(\d+)\] (Start voice=\S+.*)$', open(a.audiolog, errors='replace').read(), re.M)]
        print(f'5. audio log: {len(starts)} Start calls with a virtual time stamp')
        hit = 0; lags = []
        sv = sorted(b for _, b, _ in starts)
        import bisect
        def has_start(vb): i = bisect.bisect_left(sv, vb); return i < len(sv) and sv[i] <= vb + 1
        for k, p, gf, vb, vn in ev:
            near = [(v, b) for v, b, _ in starts if vb <= b <= vb + 1]
            if near:
                hit += 1
                v = near[0][0]; first_pass = ((v - t0) // 10000000 + 1) * 480            # the first pass due after the call
                lags.append((first_pass - (vn - t0) * 48000 // 1000000000) / 48.0)
        if ev:
            ctl = [vb + d for _, _, _, vb, _ in ev for d in (-50, -37, -22, 22, 37, 50)]      # the same events moved by 0.37 .. 0.83 s
            chit = sum(1 for vb in ctl if has_start(vb))
            print(f'   {hit} of {len(ev)} damage events ({100 * hit / len(ev):.0f}%) have a source-voice Start in their own or the next vblank; of {len(ctl)} control vblanks {chit} have one ({100 * chit / len(ctl):.0f}%)')
            if lags: print(f'   the first such voice enters the WAV {np.mean(lags):.1f} ms after the event instant on average (min {min(lags):.1f}, max {max(lags):.1f}); by construction at most one 10 ms pass after a call made within two 16.7 ms frames')
            if len(ev) >= 8 and not hit / len(ev) > 1.5 * chit / len(ctl): fail.append(f'voice starts do not cluster at the damage events ({hit}/{len(ev)} against {chit}/{len(ctl)} at control vblanks)')
    if fail:
        for f in fail: print('d18_sync: CHECK FAILED:', f, file=sys.stderr)
        sys.exit(2)
    print('d18_sync: all checks passed')


if __name__ == '__main__':
    main()
