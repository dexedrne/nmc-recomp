#!/usr/bin/env python3
"""B3 step 2a: independent RTTI recovery straight from the exe (does not read S7 pe.pkl).
Finds every Complete Object Locator (signature 1, pSelf == own RVA), type descriptor, class hierarchy descriptor with
its base class array, every vtable (qword that equals COL VA, vtable starts 8 bytes later), and slots (consecutive qwords
pointing into executable sections). Output rtti.pkl."""
import sys, pickle, struct, collections, json
sys.path.insert(0, '/home/user/projects/kh2-nmc-analysis/scratch/namedb2')
from lib import *

im = Img()
img = im.img
ra, rb = im.sec_range('.rdata')
da, db = im.sec_range('.data')
print('rdata', hex(ra), hex(rb))

# 1. COLs: 24-byte structs in .rdata, 4-aligned
cols = {}
for r in range(ra, rb - 24, 4):
    if struct.unpack_from('<I', img, r)[0] != 1:
        continue
    sig, off, cd, td, chd, pself = struct.unpack_from('<6I', img, r)
    if pself != r:
        continue
    if not (ra <= td < db and ra <= chd < rb):
        continue
    cols[r] = dict(off=off, cd=cd, td=td, chd=chd)
print('COLs', len(cols))

# 2. type descriptors
tds = {}
for c in cols.values():
    td = c['td']
    if td in tds:
        continue
    name = im.cstr(td + 16, 1024)
    tds[td] = name.decode('latin1')
print('type descriptors referenced by COLs', len(tds))

# all type descriptors anywhere (S7 counted 1,115): scan for '.?A' strings preceded by 16 bytes of header
all_td = {}
i = 0
RV = 0x5d74f0
while True:
    i = img.find(b'.?A', ra, db) if i == 0 else img.find(b'.?A', i + 1, db)
    if i < 0:
        break
    td = i - 16
    # type descriptor: pVFTable ptr (8) + spare (8, zero) + name; td must be 16-aligned in practice
    if td % 8 != 0:
        continue
    if im.u64(td + 8) != 0:
        continue
    if im.u64(td) != BASE + RV:
        continue
    nm = im.cstr(i, 1024)
    if not nm.endswith(b'@@') and not nm.endswith(b'@'):
        continue
    all_td[td] = nm.decode('latin1')
print('type descriptors by string scan', len(all_td))

# 3. class hierarchy descriptors and base class arrays
chds = {}
for col, c in cols.items():
    chd = c['chd']
    if chd in chds:
        continue
    sig, attr, n, arr = struct.unpack_from('<4I', img, chd)
    assert sig == 0, hex(chd)
    bases = []
    for k in range(n):
        bcd = im.u32(arr + 4 * k)
        btd, ncont, mdisp, pdisp, vdisp, battr, bchd = struct.unpack_from('<IIiiiII', img, bcd)
        bases.append(dict(td=btd, ncontained=ncont, mdisp=mdisp, pdisp=pdisp, vdisp=vdisp, attr=battr, chd=bchd, bcd=bcd))
    chds[chd] = dict(attr=attr, n=n, arr=arr, bases=bases)
    for b in bases:
        if b['td'] not in all_td:
            all_td[b['td']] = im.cstr(b['td'] + 16, 1024).decode('latin1')
print('CHDs', len(chds), 'type descriptors after bases', len(all_td))

# 4. vtables: qword == BASE+col at an 8-aligned address; vtable = addr+8
colva = {BASE + c: c for c in cols}
vtables = {}   # vt rva -> col rva
for scan_a, scan_b in ((ra, rb), (da, db)):
    for r in range(scan_a, scan_b - 8, 8):
        v = struct.unpack_from('<Q', img, r)[0]
        if v in colva:
            vtables.setdefault(r + 8, colva[v])
print('vtables', len(vtables))
# a COL could be referenced twice (the same vtable duplicated); check
cc = collections.Counter(vtables.values())
print('COLs with >1 vtable', sum(1 for v in cc.values() if v > 1), 'COLs w/o vtable', len(cols) - len(cc))

# 5. slots: consecutive qwords pointing into exec sections, stop at non-code
vt_slots = {}
funcs_all = set()
for vt, col in vtables.items():
    slots = []
    r = vt
    while True:
        v = struct.unpack_from('<Q', img, r)[0]
        rv = v - BASE
        if 0 < rv < im.size and im.is_exec(rv):
            slots.append(rv)
            r += 8
        else:
            break
        # stop if we run into another vtable's COL pointer (non-code anyway) or another vtable start
        if r in vtables:
            break
    vt_slots[vt] = slots
print('slots total', sum(len(s) for s in vt_slots.values()), 'unique targets', len({x for s in vt_slots.values() for x in s}))
pickle.dump(dict(cols=cols, tds=tds, all_td=all_td, chds=chds, vtables=vtables, vt_slots=vt_slots),
            open(HERE + 'rtti.pkl', 'wb'))
