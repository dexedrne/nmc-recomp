#!/usr/bin/env python3
"""Numeric report on a WAV written by KHNMC_AUDIO=wav:<file> (nothing is played).

usage: audio_wav_report.py out.wav [--ref decoded.f32 [--ref-rate 48000]] [--every 10]

Prints duration, peak / RMS / clipping, silent vs non-silent time, RMS over time, spectral sanity of the non-silent part
(flatness: ~0.56 for white noise, far lower for music or speech; centroid; band energies; roll-off), a seam test (extra
discontinuity at the 480-frame processing-pass boundaries would mean a mixer bug) and a click count.  With --ref (raw float32
stereo, e.g. an ffmpeg decode of the same stream) it locates a few output windows in the reference, fits
out = a * refL + b * refR with a fractional delay and reports how much of the output the reference explains.
"""
import sys, wave, argparse
import numpy as np

def load(path):
    w = wave.open(path, 'rb')
    assert w.getsampwidth() == 2, 'expected 16-bit PCM'
    ch, rate, n = w.getnchannels(), w.getframerate(), w.getnframes()
    x = np.frombuffer(w.readframes(n), dtype='<i2').reshape(-1, ch)
    return x, rate

def db(v):
    return -999.0 if v <= 0 else 20.0 * np.log10(v)

def spectral(x, rate, win=8192):
    """per-window flatness / centroid of the non-silent windows of a mono float signal"""
    hann = np.hanning(win)
    flat, cent, spec_sum, used = [], [], np.zeros(win // 2 + 1), 0
    for i in range(0, len(x) - win + 1, win):
        seg = x[i:i + win]
        if np.sqrt(np.mean(seg * seg)) < 1e-3:
            continue
        p = np.abs(np.fft.rfft(seg * hann)) ** 2
        p = p[1:]                                   # drop DC
        flat.append(np.exp(np.mean(np.log(p + 1e-30))) / (np.mean(p) + 1e-30))
        f = np.fft.rfftfreq(win, 1.0 / rate)[1:]
        cent.append(np.sum(f * p) / np.sum(p))
        spec_sum[1:] += p
        used += 1
    return np.array(flat), np.array(cent), spec_sum, used

def frac_shift(x, d):
    """delay x by d samples (fractional) with an FFT phase ramp"""
    n = len(x)
    f = np.fft.rfftfreq(n)
    return np.fft.irfft(np.fft.rfft(x) * np.exp(-2j * np.pi * f * d), n)

def ncc_search(sm, rm, span):
    """best normalised cross-correlation of the window sm inside rm: (position, score)"""
    n = 1 << int(np.ceil(np.log2(len(rm) + span)))
    m = len(rm) - span + 1
    cc = np.fft.irfft(np.fft.rfft(rm, n) * np.conj(np.fft.rfft(sm, n)), n)[:m]
    cs = np.cumsum(np.concatenate([[0.0], rm * rm]))
    en = np.sqrt(np.maximum(cs[span:span + m] - cs[:m], 0.0)) + 1e-9
    sc = cc / (en * np.sqrt(np.sum(sm * sm)) + 1e-12)
    k = int(np.argmax(sc))
    return k, float(sc[k])

def fit(seg, ref, pos, span):
    """out channel = a * refL + b * refR with a fractional delay; returns per channel (explained dB, coefficients, delay)"""
    res = [(-999.0, None, 0.0), (-999.0, None, 0.0)]
    for d in np.arange(-4.0, 4.01, 0.05):
        rl = frac_shift(ref[pos - 8:pos + span + 8, 0].astype(np.float64), d)[8:-8]
        rr = frac_shift(ref[pos - 8:pos + span + 8, 1].astype(np.float64), d)[8:-8]
        A = np.stack([rl, rr], axis=1)[256:-256]
        for c in (0, 1):
            y = seg[256:-256, c]
            coef, *_ = np.linalg.lstsq(A, y, rcond=None)
            e = y - A @ coef
            snr = 10 * np.log10(np.sum(y * y) / (np.sum(e * e) + 1e-30))
            if snr > res[c][0]:
                res[c] = (snr, coef, d)
    return res

def ref_check(out, rate, ref, runs, span=16384):
    """follow the output through the reference: every 4 s a window is located (globally for the first, then near the
    position a steady 1:1 playback predicts); reports continuity and a least-squares fit at a few windows"""
    x = out.astype(np.float64) / 32768.0
    refm = np.asarray(ref[:, 0], dtype=np.float64) + np.asarray(ref[:, 1], dtype=np.float64)
    prev, track, lost = None, [], 0
    for s, e in runs:
        for t0 in np.arange(s + 1.0, e - 1.0, 4.0):
            i0 = int(t0 * rate); seg = x[i0:i0 + span]; sm = seg[:, 0] + seg[:, 1]
            if len(sm) < span or np.sqrt(np.mean(sm * sm)) < 2e-4:
                continue
            if prev is None:
                best = (0, -1.0); chunk = 1 << 22
                for c0 in range(0, max(1, len(refm) - span), chunk - span):
                    r = refm[c0:c0 + chunk]
                    if len(r) < span: break
                    k, sc = ncc_search(sm, r, span)
                    if sc > best[1]: best = (c0 + k, sc)
                pos, sc = best
            else:
                pred = prev[1] + (i0 - prev[0]); lo = max(0, pred - int(1.5 * rate)); hi = min(len(refm), pred + int(1.5 * rate) + span)
                if hi - lo < span: lost += 1; continue
                k, sc = ncc_search(sm, refm[lo:hi], span); pos = lo + k
            if sc > 0.9:
                track.append((t0, i0, pos, sc, 0 if prev is None else (pos - prev[1]) - (i0 - prev[0]))); prev = (i0, pos)
            else:
                lost += 1
    if not track:
        print('  reference: no window of the output was found in the reference'); return
    off = [(p - i) / rate for _, i, p, _, _ in track]
    print(f'  reference: {len(track)} windows located, {lost} not located; output {track[0][0]:.1f}..{track[-1][0]:.1f} s plays reference {track[0][2] / rate:.3f}..{track[-1][2] / rate:.3f} s')
    print(f'  reference time - output time: {min(off):+.4f} .. {max(off):+.4f} s; largest jump between consecutive windows {max(abs(d) for *_, d in track)} frames; correlation min {min(t[3] for t in track):.4f} median {np.median([t[3] for t in track]):.4f}')
    for t0, i0, pos, sc, _ in track[::max(1, len(track) // 4)][:5]:
        if pos < 8 or pos + span + 8 > len(ref): continue
        res = fit(x[i0:i0 + span], ref, pos, span)
        print(f'  fit at output {t0:.1f} s: ' + '; '.join(f'{"LR"[c]} = {res[c][1][0]:+.4f}*refL {res[c][1][1]:+.4f}*refR (delay {res[c][2]:+.2f} frames), residual {res[c][0]:.1f} dB below the signal' for c in (0, 1)))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('wav'); ap.add_argument('--ref'); ap.add_argument('--ref-rate', type=int, default=48000); ap.add_argument('--every', type=int, default=10)
    a = ap.parse_args()
    x, rate = load(a.wav)
    n, ch = x.shape
    xf = x.astype(np.float64) / 32768.0
    print(f'file {a.wav}: {n} frames, {ch} ch, {rate} Hz, {n / rate:.2f} s')
    peak = np.max(np.abs(x)) if n else 0
    clipped = int(np.sum((x == 32767) | (x == -32768)))
    rms = np.sqrt(np.mean(xf * xf)) if n else 0.0
    print(f'peak {peak / 32768.0:.4f} ({db(peak / 32768.0):.1f} dBFS), rms {rms:.5f} ({db(rms):.1f} dBFS), full-scale (clipped) samples {clipped}, DC L {np.mean(xf[:, 0]):+.6f} R {np.mean(xf[:, -1]):+.6f}')
    nz = np.any(x != 0, axis=1)
    print(f'non-silent (any non-zero sample): {np.sum(nz) / rate:.2f} s; exact digital silence: {(n - np.sum(nz)) / rate:.2f} s')
    # per-second RMS
    secs = n // rate
    per = np.sqrt(np.mean(xf[:secs * rate].reshape(secs, rate, ch) ** 2, axis=(1, 2))) if secs else np.array([])
    loud = per > 10 ** (-60 / 20)
    print(f'seconds with RMS above -60 dBFS: {int(np.sum(loud))} of {secs}; above -40 dBFS: {int(np.sum(per > 0.01))}')
    if secs:
        runs, start = [], None
        for i, l in enumerate(list(loud) + [False]):
            if l and start is None: start = i
            if not l and start is not None: runs.append((start, i)); start = None
        print('audible runs (s): ' + ', '.join(f'{s}-{e}' for s, e in runs[:40]) + (' ...' if len(runs) > 40 else ''))
        print(f'RMS over time (dBFS, every {a.every} s window mean): ' + ' '.join(f'{i}:{db(np.sqrt(np.mean(per[i:i + a.every] ** 2))):.0f}' for i in range(0, secs, a.every)))
    mono = xf.mean(axis=1)
    flat, cent, spec, used = spectral(mono, rate)
    if used:
        f = np.fft.rfftfreq(8192, 1.0 / rate)
        tot = np.sum(spec)
        bands = [(20, 250), (250, 2000), (2000, 8000), (8000, 16000), (16000, 24000)]
        be = ' '.join(f'{lo}-{hi}Hz:{100 * np.sum(spec[(f >= lo) & (f < hi)]) / tot:.1f}%' for lo, hi in bands)
        roll = f[np.searchsorted(np.cumsum(spec) / tot, 0.99)]
        print(f'spectrum of {used} non-silent 8192-frame windows: flatness median {np.median(flat):.4f} (p10 {np.percentile(flat, 10):.4f}, p90 {np.percentile(flat, 90):.4f}; white noise is about 0.56), centroid median {np.median(cent):.0f} Hz, 99% roll-off {roll:.0f} Hz')
        print(f'band energy: {be}')
    else:
        print('spectrum: no non-silent window')
    # seam test: second difference at pass boundaries (every 480 frames) against everywhere else, non-silent region only
    d2 = np.abs(np.diff(mono, 2))
    idx = np.arange(len(d2)) + 1
    act = (np.abs(mono[1:-1]) > 1e-4)
    for period, label in ((480, '480-frame pass boundary'), (1024, '1024-frame stream buffer')):
        on = act & (idx % period == 0)
        off = act & (idx % period != 0)
        if np.sum(on) > 100:
            print(f'seam test ({label}): mean |2nd difference| at boundaries {np.mean(d2[on]):.6f}, elsewhere {np.mean(d2[off]):.6f}, ratio {np.mean(d2[on]) / (np.mean(d2[off]) + 1e-30):.3f}')
    # clicks: a sample step far outside the local level
    d1 = np.abs(np.diff(mono))
    blk = 2048
    nb = len(d1) // blk
    if nb:
        loc = np.sqrt(np.mean(d1[:nb * blk].reshape(nb, blk) ** 2, axis=1)) + 1e-5
        big = d1[:nb * blk].reshape(nb, blk) > (12.0 * loc[:, None] + 0.02)
        print(f'clicks (step > 12x the local RMS step and > 0.02): {int(np.sum(big))}')
    lr = np.corrcoef(xf[nz, 0], xf[nz, -1])[0, 1] if ch == 2 and np.sum(nz) > 10 else float('nan')
    print(f'L/R correlation over the non-silent part: {lr:.3f}')
    if a.ref:
        ref = np.memmap(a.ref, dtype='<f4', mode='r').reshape(-1, 2)
        print(f'reference {a.ref}: {len(ref)} frames ({len(ref) / a.ref_rate:.1f} s)')
        ref_check(x, rate, ref, runs if secs else [])

if __name__ == '__main__':
    main()
