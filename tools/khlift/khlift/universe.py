"""Canonical function universe loader.

Reads scratch/universe/F.pkl (the pickle written by the universe pipeline; one dict per function) and the PE image, and
produces plain `Fn` records.  Everything here is derived from the owner's local exe; nothing game-derived is stored in the
repository (only this loader).
"""
import os, pickle, collections
from . import BASE

UNIVERSE_DIR = os.environ.get('KH_UNIVERSE', '/home/user/projects/kh2-nmc-analysis/scratch/universe')
EXE = os.environ.get('KH_EXE', '/home/user/projects/kh2-nmc-analysis/pc/kh2fm.exe')

class Fn:
    __slots__ = ('jt_ext', 'rva', 'va', 'lo', 'hi', 'kind', 'nep', 'data', 'jtabs', 'unres', 'name', 'funclet', 'imports',
                 'calls', 'tails', 'ranges', 'id', 'conf', 'flags')
    def __repr__(self): return 'Fn(%x %s %d)' % (self.rva, self.kind, self.hi - self.lo)

def load():
    """-> list[Fn] sorted by rva (all 38,455 universe functions, including nep units)."""
    F = pickle.load(open(os.path.join(UNIVERSE_DIR, 'F.pkl'), 'rb'))
    out = []
    for x in F:
        f = Fn()
        f.id = x['id']; f.rva = x['start']; f.va = BASE + f.rva
        ex = x['extents']; assert len(ex) == 1
        f.lo, f.hi = BASE + ex[0][0], BASE + ex[0][1]
        f.ranges = [(BASE + a, BASE + b) for a, b in x['ranges']]
        f.kind = x['kind']; f.conf = x['confidence']; f.flags = x['flags']
        f.nep = bool(x['r'].get('nep_unit'))
        f.data = sorted((BASE + a, BASE + b, k) for a, b, k in x['data'])
        f.jtabs = {}; f.jt_ext = {}
        for jt in x['r']['jtabs']:
            f.jtabs[BASE + jt['site']] = [BASE + t for t in jt['targets']]
            f.jt_ext[BASE + jt['site']] = (BASE + jt['table'], jt['n'], jt['base'])
        f.unres = [BASE + a for a in x['r']['unres_ijmp']]
        f.name = x['name'] or ''; f.funclet = x['funclet_kind']
        f.imports = x['imports']
        f.calls = [BASE + c for c in x['calls']]; f.tails = [BASE + c for c in x['tails']]
        out.append(f)
    out.sort(key=lambda f: f.rva)
    global JT_EXTENDED
    JT_EXTENDED = extend_jump_tables(out, image())
    return out

_img = None
JT_EXTENDED = []
def image():
    """memory-mapped PE image as bytes (offset = RVA)."""
    global _img
    if _img is None:
        import pefile
        pe = pefile.PE(EXE, fast_load=True)
        _img = bytes(pe.get_memory_mapped_image())
        global IMGSZ
    return _img

def image_size():
    import pefile
    pe = pefile.PE(EXE, fast_load=True)
    return (pe.OPTIONAL_HEADER.SizeOfImage + 0xfff) & ~0xfff


def extend_jump_tables(fs, img):
    """The universe bounds a table by the nearest cmp/and; in 1 known case (f_472110, index = sum of three flag bits) the real
    table is larger and the tail was classed `unreached-data`.  Extend each table over the following 4-byte RVA entries while the
    entry lies inside an `unreached-data` range of the function (the universe's 2 unclassified data chunks) and its target
    is inside the function extent but not inside data.  Returns the list of (rva, site, old_n, new_n)."""
    import struct
    changes = []
    for f in fs:
        for site, (tab, n, base) in list(f.jt_ext.items()):
            if base: continue
            dr = [(a, b) for a, b, k in f.data if k == 'unreached-data']
            def in_dr(a): return any(lo <= a < hi for lo, hi in dr)
            k = n; ext = []
            while in_dr(tab + 4 * k) and in_dr(tab + 4 * k + 3):
                v = struct.unpack_from('<i', img, tab + 4 * k - BASE)[0]
                tgt = BASE + v
                if not (f.lo <= tgt < f.hi) or any(a <= tgt < b for a, b, kk in f.data): break
                ext.append(tgt); k += 1
            if ext:
                f.jtabs[site] = sorted(set(f.jtabs[site]) | set(ext)); changes.append((f.rva, site - BASE, n, k))
    return changes
