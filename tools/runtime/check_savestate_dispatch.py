"""Check the two frame-1300 census routes (40 preceding freeze cycles vs zero).

Pass the sequence and reference r01 directories from run-V58B/final-routes.sh.
The CPU comparison reads both private censuses. Run this check through heavy.sh.
"""
import filecmp
import re
import sys
from pathlib import Path

from compare_savestate_census import compare


def check(sequence, reference):
    logs = []
    for path in (sequence, reference):
        if (path / 'exit').read_text().strip() != '0':
            raise ValueError(f'{path}: game did not exit successfully')
        logs.append((path / 'run.out').read_bytes().splitlines())

    expected = [*range(900, 920), *range(1160, 1180), 1300]
    for lines, frames in zip(logs, (expected, [1300])):
        dispatch = [int(m[1]) for line in lines if
                    (m := re.fullmatch(rb'kh: savestate-game-dispatch frame=(\d+) rebuilt=1', line))]
        accepted = [int(m[1]) for line in lines if
                    (m := re.fullmatch(rb'kh: savestate-rendezvous frame=(\d+) epoch=\d+ workers=\d+ missing=0 gate=1 elapsed-us=\d+', line))]
        if dispatch != frames or accepted != frames:
            raise ValueError('game dispatcher or rendezvous frame sequence differs')
    rebuilds = [line for line in logs[0] if line.startswith(b'kh: savestate-fiber-rebuild ')]
    if (len(rebuilds) != len(expected) or rebuilds[0] != b'kh: savestate-fiber-rebuild replaced=6 registered=6'
            or any(line != b'kh: savestate-fiber-rebuild replaced=4 registered=4' for line in rebuilds[20:40])):
        raise ValueError('field or battle fiber rebuild missing')
    for prefix, count in ((b'kh: savestate-frame ', 1700), (b'kh: gfx frame=', 1701)):
        a, b = ([line for line in lines if line.startswith(prefix)] for lines in logs)
        if len(a) != count or a != b:
            raise ValueError(f'{prefix.decode()}: frame records differ or missing')
    for name in ('files.log', 'phase.log', 'root/field.wav'):
        if not filecmp.cmp(sequence / name, reference / name, shallow=False):
            raise ValueError(f'{name}: differs')
    if compare(sequence / 'root/end.cpu', reference / 'root/end.cpu'):
        raise ValueError('guest memory differs after 40 dispatcher restarts')
    print('41 cut frames, every game and present record, phase, files and PCM matched')


if __name__ == '__main__':
    check(*(Path(arg) for arg in sys.argv[1:]))
