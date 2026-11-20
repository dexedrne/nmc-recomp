"""Build the full MU Summit repair from the public pin and an existing finish pack."""
import argparse
from pathlib import Path
import shutil
import struct
import tempfile

from room_sweep import bar_entries

ROOM = Path('ard/us/mu07.ard')
TABLE = Path('00objentry.bin')
ROW_SIZE = 96


def require(ok, reason):
    if not ok:
        raise ValueError(reason)


def opponent(room):
    matches = [(off, size) for kind, name, off, size in bar_entries(room)
               if kind == 12 and name == 'b_60']
    require(len(matches) == 1, 'mu07-b_60-member-conflict')
    off, size = matches[0]
    # One spawn group, two entities. Serial 2 is the player, serial 6 the duel opponent.
    require(size == 8 + 44 + 2 * 64 and off + size <= len(room)
            and struct.unpack_from('<IIBBh4hI', room, off) ==
            (2, 1, 1, 0, 236, 2, 0, 0, 0, 0), 'mu07-b_60-group-conflict')
    player = off + 8 + 44
    boss = player + 64
    require(struct.unpack_from('<I', room, player)[0] == 566
            and struct.unpack_from('<h', room, player + 30)[0] == 2
            and struct.unpack_from('<h', room, boss + 30)[0] == 6, 'mu07-b_60-entity-conflict')
    return boss


def repair(pin, finish, clean):
    stock = clean.read_bytes()
    public = (pin / ROOM).read_bytes()
    room = bytearray(((finish / ROOM) if (finish / ROOM).exists() else (pin / ROOM)).read_bytes())
    pos = opponent(room)
    for blob, expected in ((stock, 1596), (public, 440), (room, 440)):
        require(struct.unpack_from('<I', blob, opponent(blob))[0] == expected,
                'mu07-b_60-opponent-conflict')

    base_path = finish / TABLE if (finish / TABLE).exists() else pin / TABLE
    table = bytearray(base_path.read_bytes())
    version, count = struct.unpack_from('<II', table)
    require(version == 3 and len(table) == 8 + count * ROW_SIZE, 'objentry-table-layout')
    rows = {struct.unpack_from('<I', table, off)[0]: table[off:off + ROW_SIZE]
            for off in range(8, len(table), ROW_SIZE)}
    require(len(rows) == count, 'objentry-duplicate-id')
    # Inherit the PC boss class and its parameters. Only this encounter gets the new record.
    boss = rows.get(440, bytearray())
    donor = rows.get(2356, bytearray())
    require(boss[4:5] == b'\x03' and boss[8:40].rstrip(b'\0') == b'H_CM060'
            and boss[40:72].rstrip(b'\0') == b'H_CM060.mset', 'mu07-boss-row-conflict')
    require(donor[8:40].rstrip(b'\0') == b'P_EX100_HTLF_BTL'
            and donor[40:72].rstrip(b'\0') == b'P_EX100_HTLF_BTL.mset', 'mu07-donor-row-conflict')
    ident = max(rows) + 1
    require(ident < 65535, 'mu07-object-id-exhausted')
    for ext in ('mdlx', 'mset', 'a.us'):
        path = Path('obj') / ('P_EX100_HTLF_BTL.' + ext)
        require(any((root / path).is_file() for root in (finish, pin)), 'mu07-donor-missing-' + ext)
    clone = bytearray(boss)
    struct.pack_into('<I', clone, 0, ident)
    clone[8:72] = donor[8:72]
    struct.pack_into('<I', table, 4, count + 1)
    table.extend(clone)
    struct.pack_into('<I', room, pos, ident)
    return room, table, ident


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('pin', 'finish', 'clean_room', 'output'):
        p.add_argument(name, type=Path)
    args = p.parse_args()
    room, table, ident = repair(args.pin, args.finish, args.clean_room)
    require(not args.output.exists(), 'mu07-output-exists')
    with tempfile.TemporaryDirectory(dir=args.output.parent) as temp:
        pack = Path(temp) / 'pack'
        shutil.copytree(args.finish, pack)
        (pack / ROOM).parent.mkdir(parents=True, exist_ok=True)
        (pack / ROOM).write_bytes(room)
        (pack / TABLE).write_bytes(table)
        pack.rename(args.output)
    print(f'Summit boss {ident}, {sum(p.is_file() for p in args.output.rglob("*"))} files')


if __name__ == '__main__':
    main()
