"""Compare a saved cut against the continuation in an independent route process."""
import argparse
import re
import wave
from pathlib import Path


def after_cut(run, tag, frame, count):
    lines = (run / 'run.out').read_bytes().splitlines()
    if (run / 'exit').read_text().strip() != '0':
        raise ValueError(f'{run} failed to finish')
    if tag == b'loaded' and not any(line.startswith(b'kh: savestate-cold-bootstrap ') for line in lines):
        raise ValueError(f'{run} lacks a fresh-process bootstrap')
    pattern = re.compile(rb'^kh: savestate-' + tag + rb' at=\d+ resumed-frame=(\d+) sample=(\d+)')
    for i, line in enumerate(lines):
        match = pattern.match(line)
        if match and int(match[1]) == frame:
            break
    else:
        raise ValueError(f'{run} lacks {tag.decode()} for frame {frame}')
    game, gfx = [], []
    for line in lines[i+1:]:
        if line.startswith(b'kh: gfx frame='):
            gfx.append(line)
        elif line.startswith(b'kh: savestate-frame '):
            game.append(line)
            if len(game) == count:
                break
    frames = [int(line.split()[2]) for line in game]
    if frames != list(range(frame + 1, frame + count + 1)) or not gfx:
        raise ValueError(f'{run} has an incomplete continuation')
    return game, gfx, int(match[2])


def compare(producer, consumer, frame, count):
    if count <= 0:
        raise ValueError('count must be positive')
    if producer.resolve() == consumer.resolve():
        raise ValueError('producer and consumer must be independent runs')
    a, b = (after_cut(path, tag, frame, count) for path, tag in
            ((producer, b'captured'), (consumer, b'loaded')))
    for records, other, label in zip(a[:2], b[:2], ('game records', 'null presents')):
        if records != other:
            raise ValueError(f'{label} differ after frame {frame}')
    # Compare all PCM from the exact sample at each cut through route exit.
    samples = []
    for run, start in ((producer, a[2]), (consumer, b[2])):
        with wave.open(str(run / 'root/field.wav')) as wav:
            wav.setpos(start)
            samples.append((wav.getparams()[:3], wav.readframes(wav.getnframes()-start)))
    if samples[0] != samples[1] or not any(samples[0][1]):
        raise ValueError(f'PCM differs or is silent after frame {frame}')
    print(f'frame {frame}: {len(a[0])} game records, {len(a[1])} presents, '
          f'{len(samples[0][1])} PCM bytes through exit equal')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('producer', type=Path)
    parser.add_argument('consumer', type=Path)
    parser.add_argument('frame', type=int)
    parser.add_argument('--count', type=int, default=120)
    args = parser.parse_args()
    compare(args.producer, args.consumer, args.frame, args.count)
