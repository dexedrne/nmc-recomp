#!/usr/bin/env python3
"""Create a private source tree using the pinned byte ranges from your own PC game and mod.

Run python3 tools/publish/prepare_sources.py OUTPUT --exe FILE --mod DIR --data DIR.
OUTPUT must be a new directory outside the checkout. Build from that private copy.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import tempfile

MARKER = re.compile(r'@KHINPUT\((array|slash|hex|message),([\w./-]+),(\d+),(\d+),([a-f0-9]{64})\)@')


def main():
    arg = argparse.ArgumentParser(description=__doc__)
    arg.add_argument('output', type=Path)
    for key in ('exe', 'mod', 'data'):
        arg.add_argument('--' + key, required=True, type=Path)
    opt = arg.parse_args()
    root = Path(__file__).resolve().parents[2]
    dest = opt.output.resolve()
    if dest.exists() or dest.is_relative_to(root):
        arg.error('choose a new output directory outside the source tree')

    def expand(match):
        fmt, source, offset, count, expected = match.groups()
        group, path = source.split('/', 1)
        if group == 'pc' and path == 'kh2fm.exe':
            target = opt.exe
        elif group in ('mod', 'data'):
            parent = getattr(opt, group).resolve()
            target = (parent / path).resolve()
            if not target.is_relative_to(parent):
                raise ValueError('input escapes its data directory')
        else:
            raise ValueError('unknown input ' + source)
        with target.open('rb') as f:
            f.seek(int(offset))
            raw = f.read(int(count))
        if hashlib.sha256(raw).hexdigest() != expected:
            raise ValueError(f'{source} at {offset} does not match the required version')
        if fmt == 'array':
            return ','.join(f'0x{v:02x}' for v in raw)
        if fmt == 'slash':
            return ''.join(f'\\x{v:02x}' for v in raw)
        if fmt == 'message':
            import sys
            sys.path.insert(0, str(root / 'tools/mod'))
            from msgdump import decode
            return json.dumps(decode(raw, 0).replace(' / ', ' '), ensure_ascii=True)[1:-1]
        return raw.hex()

    with tempfile.TemporaryDirectory(dir=dest.parent) as tmp:
        tree = Path(tmp) / 'sources'
        shutil.copytree(root, tree, ignore=shutil.ignore_patterns('.git', '__pycache__'))
        for part in tree.rglob('*.kh-input'):
            out = part.with_suffix('')
            if out.exists():
                raise ValueError(f'output exists: {out}')
            text = MARKER.sub(expand, part.read_text())
            if '@KHINPUT(' in text:
                raise ValueError(f'invalid reference in {part}')
            out.write_text(text)
            shutil.copymode(part, out)
            part.unlink()
        tree.rename(dest)


if __name__ == '__main__':
    main()
