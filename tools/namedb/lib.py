"""Shared helpers for name DB v2. Read-only on the exe and on every input outside scratch/namedb2."""
import struct, bisect, json, csv, re, os, pickle, collections, subprocess
HERE = os.path.dirname(os.path.abspath(__file__)) + '/'
SCR = '/home/user/projects/kh2-nmc-analysis/scratch/'
EXE = '/home/user/projects/kh2-nmc-analysis/pc/kh2fm.exe'
UNI = SCR + 'universe/skeptic/corrected/'   # corrected canonical universe: 38,451 functions
S7 = SCR + 'S7-name-db/'
BASE = 0x140000000


class Img:
    def __init__(self, path=EXE):
        import pefile
        self.raw = open(path, 'rb').read()
        self.pe = pefile.PE(data=self.raw, fast_load=True)
        self.img = bytes(self.pe.get_memory_mapped_image())
        self.secs = [(s.Name.rstrip(b'\0').decode(), s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData,
                      s.SizeOfRawData, s.Characteristics) for s in self.pe.sections]
        self.exec = [(a, a + v) for n, a, v, po, ps, ch in self.secs if ch & 0x20000000]
        self.size = len(self.img)

    def sec(self, rva):
        for n, a, v, po, ps, ch in self.secs:
            if a <= rva < a + max(v, ps):
                return n
        return None

    def is_exec(self, rva):
        return any(a <= rva < b for a, b in self.exec)

    def u8(self, r): return self.img[r]
    def u16(self, r): return struct.unpack_from('<H', self.img, r)[0]
    def u32(self, r): return struct.unpack_from('<I', self.img, r)[0]
    def i32(self, r): return struct.unpack_from('<i', self.img, r)[0]
    def u64(self, r): return struct.unpack_from('<Q', self.img, r)[0]

    def cstr(self, r, maxn=512):
        e = self.img.find(b'\0', r, r + maxn)
        if e < 0:
            e = r + maxn
        return self.img[r:e]

    def sec_range(self, name):
        for n, a, v, po, ps, ch in self.secs:
            if n == name:
                return a, a + v
        raise KeyError(name)


def load_universe():
    """corrected universe (functions.json): list of dicts, keyed by start_rva"""
    d = json.load(open(UNI + 'functions.json'))
    return d['functions']


def hx(v):
    return '0x%x' % v


def undname(mangled_list):
    """demangle RTTI type descriptor names through llvm-undname (names like '.?AVFoo@@')"""
    out = {}
    names = sorted(set(mangled_list))
    for i in range(0, len(names), 100):
        chunk = names[i:i + 100]
        args = ['??_R0' + n[1:] + '@8' for n in chunk]
        r = subprocess.run(['llvm-undname'] + args, capture_output=True, text=True).stdout
        blocks = [b for b in r.split('\n\n') if b.strip()]
        assert len(blocks) == len(chunk), (len(blocks), len(chunk))
        for n, b in zip(chunk, blocks):
            ls = b.strip().split('\n')
            out[n] = re.sub(r" `RTTI Type Descriptor'$", "", ls[-1].strip())
    return out


def clean_type(q):
    return re.sub(r'^(class|struct|union|enum) ', '', q)
