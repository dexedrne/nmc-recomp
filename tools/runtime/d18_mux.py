#!/usr/bin/env python3
"""d18_mux.py - join the picture stream of a KHNMC_CLOCK=frames run with its sound, sample-exactly, and check the cadence.

  d18_mux.py --video V.mp4 --index V.tsv --wav A.wav --out OUT.mp4 [--audio-bitrate 192k] [--check-only]

V.mp4 is the H.264 encode of the raw stream KHNMC_GFX_VIDEO wrote (one picture per virtual vblank, 60 per second of game time), V.tsv
its index (picture, vblank, virtual_ns, present, game_frame, new, audio_sample), A.wav the KHNMC_AUDIO=wav: file of the SAME run (its
sample 0 is the virtual time at which the device clock started; audio_sample is the WAV sample frame of each picture's vblank).
The sound is cut at exactly audio_sample of picture 0, for pictures * 800 sample frames, and muxed without re-encoding the video.
Checked before anything is written: every picture is one vblank after the previous one, its audio_sample is 800 * k after the first
(1 sample of rounding allowed), and the WAV is long enough.  Any failure: exit 2, nothing written."""
import argparse, struct, subprocess, sys, wave

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--video', required=True); ap.add_argument('--index', required=True); ap.add_argument('--wav', required=True); ap.add_argument('--out', required=True)
    ap.add_argument('--audio-bitrate', default='192k'); ap.add_argument('--check-only', action='store_true')
    a = ap.parse_args()
    rows = []
    with open(a.index) as f:
        head = f.readline().rstrip('\n').split('\t')
        if head != ['picture', 'vblank', 'virtual_ns', 'present', 'game_frame', 'new', 'audio_sample']:
            sys.exit(f'd18_mux: unexpected index header {head}')
        for ln in f:
            p = ln.split('\t')
            rows.append(tuple(int(x) for x in p))
    if not rows:
        sys.exit('d18_mux: the index has no pictures')
    n = len(rows); v0, t0, s0 = rows[0][1], rows[0][2], rows[0][6]
    bad = []
    if s0 < 0:
        sys.exit('d18_mux: picture 0 has no audio sample (the device clock was not on the virtual clock)')
    worst = 0
    for k, (pic, vb, vns, pres, gf, new, asmp) in enumerate(rows):
        if pic != k: bad.append(f'picture {k}: numbered {pic}')
        if vb != v0 + k: bad.append(f'picture {k}: vblank {vb}, expected {v0 + k}')
        exp_ns = (vb * 1000000000 // 60) - (v0 * 1000000000 // 60)
        if vns - t0 != exp_ns: bad.append(f'picture {k}: virtual time +{vns - t0} ns, expected +{exp_ns}')
        d = (asmp - s0) - 800 * k
        worst = max(worst, abs(d))
        if abs(d) > 1: bad.append(f'picture {k}: audio sample +{asmp - s0}, expected {800 * k}')
        if len(bad) > 8: break
    with wave.open(a.wav, 'rb') as w:
        ch, width, rate, frames = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
    if (ch, width, rate) != (2, 2, 48000): bad.append(f'WAV is {ch} ch {8 * width} bit {rate} Hz, expected 2 ch 16 bit 48000 Hz')
    end = s0 + 800 * n
    if frames < end: bad.append(f'WAV has {frames} sample frames, the pictures need up to {end}')
    new_pics = sum(1 for r in rows if r[5]); rep = n - new_pics
    print(f'd18_mux: {n} pictures, virtual vblanks {v0}..{rows[-1][1]}, {n / 60:.3f} s of game time; new pictures {new_pics}, repeats {rep}; presents {rows[0][3]}..{rows[-1][3]}, game frames {rows[0][4]}..{rows[-1][4]}')
    print(f'd18_mux: sound: WAV sample frames {s0}..{end} of {frames} ({s0 / 48000:.4f} s .. {end / 48000:.4f} s); picture k is sample {s0} + 800*k, largest deviation {worst} sample frame(s)')
    if bad:
        for b in bad[:10]: print('d18_mux: CHECK FAILED:', b, file=sys.stderr)
        sys.exit(2)
    print('d18_mux: cadence checks passed (every picture one vblank and 800 sample frames after the previous one)')
    if a.check_only: return
    cmd = ['/usr/bin/ffmpeg', '-nostdin', '-loglevel', 'error', '-y', '-i', a.video, '-i', a.wav, '-filter_complex',
           f'[1:a]atrim=start_sample={s0}:end_sample={end},asetpts=PTS-STARTPTS[a]', '-map', '0:v', '-map', '[a]', '-c:v', 'copy', '-c:a', 'aac', '-b:a', a.audio_bitrate,
           '-movflags', '+faststart', a.out]
    r = subprocess.run(cmd)
    if r.returncode: sys.exit(f'd18_mux: ffmpeg failed with status {r.returncode}')
    print('d18_mux: wrote', a.out)

if __name__ == '__main__':
    main()
