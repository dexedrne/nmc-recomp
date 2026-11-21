"""Merge the Holo access programs into a fresh copy of the shared PC overlay."""
from pathlib import Path
from hashlib import sha256
import shutil
import struct
import sys
import tempfile

work = Path('/home/user/work')
tools = work / 'nmc-decomp/tools'
sys.path[:0] = [str(tools), str(tools / 'rooms')]
from formats import bar, directory_record, programs, require
from records import arguments, cmd, commands, values

stock = work / 'khnmc-build/data-full'
shared = work / 'khnmc-packs/rmc'


def member(data, name):
    entry, = [e for e in bar(data) if e.kind == 13 and e.name.rstrip(b'\0') == name.encode()]
    return entry


def subset(body, group):
    return b''.join(cmd(t, a) for t, a in commands(body) if t == 'AreaSettings'
                    or (t == 'Spawn' and values(t, a) == [group]))


def merge(room, section, changes, current=None):
    path = Path('ard/us') / (room + '.ard')
    base = (stock / path).read_bytes()
    if current is None:
        current = (shared / path).read_bytes() if (shared / path).exists() else base
    target = member(current, section)
    base_rows = dict(programs(member(base, section).data(base)))
    rows = programs(target.data(current))
    for key in changes:
        require(sum(k == key for k, _ in rows) == 1 and dict(rows)[key] == base_rows[key],
                f'conflicting existing edit in {room} {section} {key}')
    payload = bytearray()
    for key, body in rows:
        body += changes.get(key, b'')
        payload.extend(struct.pack('<hH', key, 4 + len(body)) + body)
    payload.extend(struct.pack('<hH', -1, 0))
    result = bytearray(current)
    result.extend(bytes(-len(result) % 16))
    directory_record(result, target.ordinal, len(result), len(payload))
    result.extend(payload)
    for a, b in zip(bar(current), bar(result)):
        if a != target:
            require(a == b and a.data(current) == b.data(result), 'sibling changed')
    return path, result


def stock_program(room, section, key):
    data = (stock / f'ard/us/{room}.ard').read_bytes()
    return dict(programs(member(data, section).data(data)))[key]


def merlin(computer):
    path = Path('ard/us/hb13.ard')
    archive = (shared / path if (shared / path).exists() else stock / path).read_bytes()
    entries = bar(archive)
    count = len(entries)
    require(not any(e.name == b'holo' for e in entries), 'Holo member already present')
    trigger, = [cmd(t, a) for t, a in commands(computer) if t == 'AreaSettings']
    _, data = merge('hb13', 'map', {1: trigger + cmd('SpawnAlt', b'm_50')}, archive)
    script_cmd = cmd('MapScript', struct.pack('<I', count))
    _, data = merge('hb13', 'evt', {16: script_cmd, 19: script_cmd}, data)

    # Trigger 3 follows wi00's SpawnAlt + UNIT::Activate convention.
    # map_set(); push 1; sub; seqz; beqz return; push.bd m_50; activate(); ret
    words = [0x4a,25, 0,1,0, 0x46,0x205, 0x47,4, 0xe0,28, 0x4a,45, 0x89]
    return path, append_script(data, b'holo-computer', words, b'm_50\0\0\0\0')


def append_script(data, name, words, constants=b''):
    entries = bar(data)
    require(not any(e.name == b'holo' for e in entries), 'existing Holo script')
    count = len(entries)
    script = struct.pack('<16s7I', name, 0, 512, 512, 3, 14, 0, 0)
    script += struct.pack(f'<{len(words)}H', *words) + constants
    boundary = 16 + 16 * count
    result = data[:boundary] + bytearray(16) + data[boundary:]
    struct.pack_into('<I', result, 4, count + 1)
    for e in entries:
        directory_record(result, e.ordinal, e.offset + 16, e.size)
    result.extend(bytes(-len(result) % 16))
    struct.pack_into('<HH4sII', result, boundary, 3, 0, b'holo', len(result), len(script))
    result.extend(script)
    return result


def archive_hub(terminal):
    path, data = merge('po00', 'map', {10: terminal})
    conditional = cmd('If', arguments('If', [6, ['MapScript', len(bar(data))]]))
    _, data = merge('po00', 'evt', {0: conditional}, data)
    # In event 0 at door 6, require both remaining selectors to be zero.
    # map_set | battle_set; beqz initialize; ret
    guard = [0x4a, 25, 0x4a, 26, 0x186, 0x47, 1, 0x89]
    initialize = [0, 0x2400, 0, 0x0a, 14]  # progress_set_flag(0x2400)
    # NMC MapJump accepts explicit world/room/door and all three saved selectors.
    for argument in (9, 0, 50, 0xffff, 0xffff, 0xffff, 1, 0, 0):
        initialize.extend([0, argument, 0])
    initialize.extend([0xca, 8, 0x89])
    return path, append_script(data, b'holo-first', guard + initialize)



def build(dest):
    require(not dest.exists() and not dest.is_symlink(), 'choose a fresh output directory')
    require(shared.resolve() not in dest.resolve().parents, 'output inside shared overlay')
    for room, digest in [
        ('hb13', 'f6b7f74babbb0ae73c61e051b35518c657181818869fb8c64d0a58e7a1061542'),
        ('po00', '357a38aafa5cebc9573c693bb9cb69a99949ac65a2432544e51a27762c1fbb2d'),
    ]:
        require(sha256((stock / f'ard/us/{room}.ard').read_bytes()).hexdigest() == digest,
                f'unexpected stock revision for {room}')
    full = subset(stock_program('po00', 'evt', 27), 'm_53')
    computer = subset(stock_program('hb13', 'map', 0), 'm_50')
    require(len(list(commands(full))) == 4 and len(list(commands(computer))) == 2, 'donor mismatch')
    changes = [archive_hub(full), merlin(computer)]
    with tempfile.TemporaryDirectory(dir=dest.parent) as stage:
        output = Path(stage) / 'pack'
        shutil.copytree(shared, output)
        for path, content in changes:
            (output / path).write_bytes(content)
        output.rename(dest)


if __name__ == '__main__':
    build(Path(sys.argv[1]))
