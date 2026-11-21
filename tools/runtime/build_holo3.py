"""Merge both demonstrated Day 193 repairs into a fresh full overlay."""
from pathlib import Path
import shutil
import struct
import sys
import tempfile

work = Path('/home/user/work')
sys.path.insert(0, str(work/'nmc-decomp/tools'))
from formats import bar, directory_record, programs, require

stock = work/'khnmc-build/data-full'
shared = work/'khnmc-packs/rmc'
path = Path('ard/us/po01.ard')
partner = Path('obj/N_PO030_AIR_RTN.mdlx')


def events(data):
    member, = [m for m in bar(data) if (m.kind, m.name, m.link) == (13, b'evt\0', 0)]
    return member, programs(member.data(data))


def merge(current):
    _, original = events((stock/path).read_bytes())
    first, replay = [body for key, body in original if key == 3]
    target, = [body for key, body in original if key == 4]
    require(target == first and first.count(struct.pack('<H', 0x2416)) == 1
            and first.replace(struct.pack('<H', 0x2416), struct.pack('<H', 0x241e)) == replay,
            'unexpected stock abort programs')
    member, rows = events(current)
    require([b for k,b in rows if k == 4] == [target], 'conflicting po01 event 4')
    offset = member.offset
    for key, body in rows:
        offset += 4
        if key == 4:
            return current[:offset] + replay + current[offset+len(body):]
        offset += len(body)
    raise ValueError('missing po01 event 4')


def guard_sheet(current):
    stock_actor = (stock/partner).read_bytes()
    def script(blob):
        entry, = [m for m in bar(blob) if (m.kind,m.name,m.link)==(3,b'b_ex',0)]
        return entry
    member = script(current)
    base = script(stock_actor).data(stock_actor)
    body = bytearray(member.data(current))
    # Both halves of the elemental-rate effect need a live status sheet.
    for pc, rate in [(12064,100), (12129,0)]:
        call = struct.pack('<11H', 0x4a,84, 0,6,0, 0,rate,0, 0x4a,311, 0x89)
        off = 16 + 2*pc
        require(base[off:off+len(call)] == call and body[off:off+len(call)] == call,
                f'conflicting sheet operation at word {pc}')
        extra = (len(body)-16)//2
        struct.pack_into('<Hh',body,off,7,extra-pc-2)
        # obj_sheet, dup, branch back for a live sheet, otherwise drop and return.
        body.extend(struct.pack('<4Hh2H',0x4a,84,0x149,0x87,pc+2-(extra+5),0xc9,0x89))
    result = bytearray(current)
    result.extend(bytes(-len(result)%16))
    directory_record(result,member.ordinal,len(result),len(body))
    result.extend(body)
    return result


def build(out):
    require(not out.exists(), 'choose a fresh output path')
    current = shared/path if (shared/path).exists() else stock/path
    with tempfile.TemporaryDirectory(dir=out.parent) as tmp:
        stage = Path(tmp)/'pack'
        shutil.copytree(shared, stage)
        (stage/path).write_bytes(merge(current.read_bytes()))
        actor = shared/partner if (shared/partner).exists() else stock/partner
        (stage/partner).write_bytes(guard_sheet(actor.read_bytes()))
        stage.rename(out)


if __name__ == '__main__':
    build(Path(sys.argv[1]))
