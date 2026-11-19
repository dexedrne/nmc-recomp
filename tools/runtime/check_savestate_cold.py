"""Compare full CPU censuses from N sequential cold starts at the requested frame."""
import argparse
import re
from pathlib import Path

from compare_savestate_census import compare


def check(batch, frame):
    runs = sorted(p for p in batch.iterdir() if re.fullmatch(r'r\d+', p.name))
    if len(runs) < 2:
        raise ValueError("cold-start check needs at least two runs")
    accepted = re.compile(rf'kh: savestate-rendezvous frame={frame} epoch=\d+ workers=\d+ missing=0 gate=1 elapsed-us=\d+')
    for run in runs:
        if (run / 'exit').read_text().strip() != '0':
            raise ValueError(f'{run}: unsuccessful game exit')
        if f'KHNMC_TEST_SAVESTATE=frame:{frame}:census:end.cpu' not in (run / 'env').read_text().splitlines():
            raise ValueError(f'{run}: unexpected census trigger')
        if not any(accepted.fullmatch(line) for line in (run / 'run.out').read_text().splitlines()):
            raise ValueError(f'{run}: missing frame {frame} cut')
        if run != runs[0] and compare(runs[0] / 'root/end.cpu', run / 'root/end.cpu'):
            raise ValueError(f'{run}: cold-start guest memory differs')
    print(f'{len(runs)} cold starts agreed at frame {frame}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('batch', type=Path)
    parser.add_argument('frame', type=int)
    args = parser.parse_args()
    check(args.batch, args.frame)
