#!/usr/bin/env python3
"""B3 step 1a: one decoding pass over every function of the corrected universe (data-in-code and unreached-data ranges
are skipped, never decoded). Records per function: rip-relative references (lea/mov/cmp/... to data), vtable-pointer
stores, indirect call shapes, and a compact instruction fingerprint. Output scan.pkl."""
import sys, pickle, struct, collections
sys.path.insert(0, '/home/user/projects/kh2-nmc-analysis/scratch/namedb2')
from lib import *
from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_OP_MEM, CS_OP_IMM, CS_OP_REG
from capstone.x86 import X86_REG_RIP

im = Img()
img = im.img
R = pickle.load(open(HERE + 'rtti.pkl', 'rb'))
vtset = set(R['vtables'])
U = load_universe()
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
md.skipdata = True

out = {}
for fi, f in enumerate(U):
    s = f['start_rva']
    # ranges minus data-in-code
    bad = []
    for d in f.get('data_in_code', []):
        bad.append((d[0], d[1]) if isinstance(d, (list, tuple)) else (d['start'], d['end']))
    for u in f.get('unreached_ranges', []):
        if isinstance(u, (list, tuple)):
            bad.append((u[0], u[1]))
        elif u.get('class', 'unreached-data') != 'unreached-code':
            bad.append((u['start'], u['end']))
    segs = []
    for a, b in f['extents']:
        cur = a
        for (x, y) in sorted(b_ for b_ in bad if b_[0] < b and b_[1] > a):
            if x > cur:
                segs.append((cur, x))
            cur = max(cur, y)
        if cur < b:
            segs.append((cur, b))
    refs = []
    vst = []
    ninsn = 0
    mnem = collections.Counter()
    lastlea = {}
    for a, b in segs:
        code = img[a:b]
        for ins in md.disasm(code, a):
            ninsn += 1
            if ins.id == 0:      # skipdata pseudo
                continue
            mn = ins.mnemonic
            mnem[mn] += 1
            for op in ins.operands:
                if op.type == CS_OP_MEM and op.mem.base == X86_REG_RIP:
                    tgt = ins.address + ins.size + op.mem.disp
                    refs.append((ins.address, tgt, mn))
                    if mn == 'lea' and tgt in vtset:
                        lastlea[ins.operands[0].reg] = (ins.address, tgt, ninsn)
                elif op.type == CS_OP_MEM and mn == 'mov' and ins is not None:
                    # store of a vtable pointer: mov [base+disp], reg where reg holds lea'd vtable
                    if len(ins.operands) == 2 and ins.operands[1].type == CS_OP_REG and ins.operands[1].reg in lastlea and ins.operands[0].type == CS_OP_MEM:
                        la, vt, n0 = lastlea[ins.operands[1].reg]
                        if ninsn - n0 <= 12:
                            vst.append((ins.address, vt, op.mem.disp, op.mem.base))
    out[s] = dict(refs=refs, vstores=vst, ninsn=ninsn, mnem=dict(mnem))
    if fi % 5000 == 0:
        print(fi, flush=True)
pickle.dump(out, open(HERE + 'scan.pkl', 'wb'))
print('done', len(out), sum(len(v['refs']) for v in out.values()), sum(len(v['vstores']) for v in out.values()))
