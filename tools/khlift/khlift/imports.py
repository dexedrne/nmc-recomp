"""PE import table -> runtime hook symbols."""
import re
from . import BASE
from . import universe as U

class Imp:
    __slots__ = ('idx', 'slot', 'dll', 'name', 'ordinal', 'sym', 'addr')

def _san(s): return re.sub(r'[^0-9A-Za-z]', '_', s)

def load_imports():
    import pefile
    pe = pefile.PE(U.EXE)
    out = []
    for d in pe.DIRECTORY_ENTRY_IMPORT:
        dll = d.dll.decode()
        short = _san(re.sub(r'\.dll$', '', dll, flags=re.I).lower())
        for i in d.imports:
            im = Imp(); im.idx = len(out); im.slot = i.address; im.dll = dll
            im.name = i.name.decode() if i.name else ''; im.ordinal = i.ordinal
            base = _san(im.name) if im.name else 'ord%d' % im.ordinal
            im.sym = 'rt_imp_%s__%s' % (short, base)
            out.append(im)
    seen = {}
    for im in out:
        if im.sym in seen: im.sym += '_%d' % im.idx
        seen[im.sym] = im
    # synthetic guest address of each import: the runtime stores these in the IAT slots (it may choose another base)
    for im in out: im.addr = KH_IMPORT_ADDR_BASE + 16 * im.idx
    return out

KH_IMPORT_ADDR_BASE = 0x7ff7f0000000   # default synthetic import address block (never dereferenced); runtime may relocate
