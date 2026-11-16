#!/usr/bin/env python3
"""B3 step 2b: class model from the independently recovered RTTI: names, direct bases, hierarchy, vtables, slot families,
inherited/overridden/new/pure classification, virtual function naming, adjustor thunks, vptr-store ctor/dtor evidence.
Reads rtti.pkl + scan.pkl + corrected universe. Output classes.pkl."""
import sys, pickle, collections, re, json, struct
sys.path.insert(0, '/home/user/projects/kh2-nmc-analysis/scratch/namedb2')
from lib import *
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

im = Img()
R = pickle.load(open(HERE + 'rtti.pkl', 'rb'))
SC = pickle.load(open(HERE + 'scan.pkl', 'rb'))
U = load_universe()
F = {f['start_rva']: f for f in U}
cols, all_td, chds, vtables, vt_slots = R['cols'], R['all_td'], R['chds'], R['vtables'], R['vt_slots']
PURECALL = 0x471a3c
assert 'VCRUNTIME140.dll!_purecall' in F[PURECALL]['imports']

dem = undname(list(all_td.values()))
tdname = {td: clean_type(dem[n]) for td, n in all_td.items()}


def logical(q):
    m = re.match(r'^(.*)::VTABLE<(?:class|struct) (.+)>$', q)
    if m:
        return m.group(2), m.group(1)
    return q, None


# ---- direct bases, from every base class array in which a type appears
direct = collections.defaultdict(dict)       # td -> {btd: virtual?}
for chd, c in chds.items():
    arr = c['bases']
    n = len(arr)

    def children(i):
        j = i + 1
        end = i + 1 + arr[i]['ncontained']
        out = []
        while j < end:
            out.append(j)
            j += 1 + arr[j]['ncontained']
        return out
    for i in range(n):
        t = arr[i]['td']
        for j in children(i):
            direct[t].setdefault(arr[j]['td'], arr[j]['pdisp'] != -1)
for td in all_td:
    direct.setdefault(td, {})

depth = {}


def dep(td, seen=()):
    if td in depth:
        return depth[td]
    d = 0
    for b in direct[td]:
        if b not in seen:
            d = max(d, 1 + dep(b, seen + (td,)))
    depth[td] = d
    return d


for td in all_td:
    dep(td)


def ancestors(td, acc=None):
    acc = set() if acc is None else acc
    for b in direct[td]:
        if b not in acc:
            acc.add(b)
            ancestors(b, acc)
    return acc


anc = {td: ancestors(td) for td in all_td}

# ---- vtables per type
classes = {}
for td, nm in all_td.items():
    q = tdname[td]
    lg, iface = logical(q)
    classes[td] = dict(td=td, mangled=nm, name=q, logical=lg, iface=iface, kind=dem[nm].split(' ')[0],
                       template='<' in q, lam='<lambda' in q, direct_bases=sorted(direct[td], key=lambda b: tdname[b]),
                       virtual_bases=[b for b, v in direct[td].items() if v], depth=depth[td], vtables=[])
chd_of = {}
for col, c in cols.items():
    chd_of[col] = chds[c['chd']]
prim = {}       # td -> slots of its offset-0 vtable
for vt, col in vtables.items():
    c = cols[col]
    classes[c['td']]['vtables'].append(dict(rva=vt, col=col, off=c['off'], slots=vt_slots[vt]))
    if c['off'] == 0:
        prim[c['td']] = vt_slots[vt]


def chain_for(td, off, col):
    """types occupying the subobject at COL offset `off` of complete type td, derived -> base"""
    arr = chd_of[col]['bases']
    ch = []
    if off == 0:
        ch.append(td)
    for b in arr[1:]:
        if b['mdisp'] == off and b['pdisp'] == -1:
            ch.append(b['td'])
    out = []
    for x in ch:
        if x not in out:
            out.append(x)
    return out


# families and slot categories
slot_info = {}        # (vt, slot) -> dict
users = collections.defaultdict(list)       # fn -> list of (td, vt, off, slot)
fam_all = collections.defaultdict(set)      # (root td, slot) -> set of (td,vt)
for td, c in classes.items():
    for v in c['vtables']:
        ch = chain_for(td, v['off'], v['col'])
        v['chain'] = ch
        v['chain_virtual'] = (len(ch) == 0)
        for s, t in enumerate(v['slots']):
            users[t].append((td, v['rva'], v['off'], s))
            # topmost chain element with a primary vtable of more than s slots
            root = None
            for cand in ch:
                if cand in prim and len(prim[cand]) > s:
                    root = cand
            synthetic = False
            if root is None:
                # no recorded primary vtable in the chain: take the topmost chain element (or the type itself)
                root = ch[-1] if ch else td
                synthetic = True
            # defining class: topmost class in chain (root->derived) whose own slot s == t (and has >s slots)
            definer = None
            for cand in reversed(ch):
                if cand in prim and len(prim[cand]) > s and prim[cand][s] == t and cand != td:
                    definer = cand
                    break
            if t == PURECALL:
                cat = 'pure'
            elif definer is not None:
                cat = 'inherited'
            elif root == td and not synthetic:
                cat = 'new'
            else:
                cat = 'overridden'
                # if some base in chain has this slot with another target it is an override; if no base had the slot, new
                if not any((b in prim and len(prim[b]) > s) for b in ch if b != td):
                    cat = 'new'
            slot_info[(v['rva'], s)] = dict(root=root, synthetic=synthetic, cat=cat, definer=definer, fn=t, td=td, off=v['off'])
            fam_all[(root, s)].add((td, v['rva']))

# counts for the family reconciliation
fam_resolved = {k for k in fam_all}
fam_by_td_slot = len(fam_all)
fam_nonsynthetic = len({(slot_info[k]['root'], k[1]) for k in slot_info if not slot_info[k]['synthetic']})
fam_synthetic = len({(slot_info[k]['root'], k[1]) for k in slot_info if slot_info[k]['synthetic']})
fam_logical = len({(logical(tdname[slot_info[k]['root']])[0], k[1]) for k in slot_info})

# ---- adjustor thunks
md = Cs(CS_ARCH_X86, CS_MODE_64)
thunk_of = {}
for t, f in F.items():
    if f['kind'] != 'thunk':
        continue
    code = im.img[t:t + min(f['size'], 24)]
    adj = 0
    tgt = None
    if f['tail_targets']:
        tgt = f['tail_targets'][0]
    # decode adjustments of rcx
    kind = 'jmp'
    for ins in md.disasm(code, t):
        if ins.mnemonic in ('add', 'sub') and ins.op_str.startswith('rcx, ') and ins.op_str.split(', ')[1].startswith(('0x', '-')) or (ins.mnemonic in ('add', 'sub') and ins.op_str.startswith('rcx, ') and ins.op_str.split(', ')[1].isdigit()):
            v = int(ins.op_str.split(', ')[1], 0)
            adj += -v if ins.mnemonic == 'sub' else v
        elif ins.mnemonic == 'lea' and ins.op_str.startswith('rcx, [rcx'):
            m = re.search(r'([+-]) (0x[0-9a-f]+|\d+)\]', ins.op_str)
            if m:
                v = int(m.group(2), 0)
                adj += v if m.group(1) == '+' else -v
        elif ins.mnemonic == 'jmp':
            if '[' in ins.op_str:
                kind = 'jmp-indirect'
            break
    thunk_of[t] = dict(adjust=adj, target=tgt, kind=kind)

# ---- vtable-pointer-store evidence -> ctor / dtor candidates per class
vt_class = {vt: (classes_td, off) for classes_td, c in classes.items() for v in c['vtables'] for vt, off in [(v['rva'], v['off'])] for classes_td in [classes_td]}
vt2td = {v['rva']: td for td, c in classes.items() for v in c['vtables']}
stores = collections.defaultdict(list)
for fn, s in SC.items():
    for (addr, vt, disp, base) in s['vstores']:
        stores[fn].append((vt, vt2td[vt], disp))

# deleting destructors: slot-0 functions that test dl,1 and call something (op delete); detect via bytes 'f6 c2 01' (test dl,1) or 'a8/...'
def is_deleting_dtor(fn):
    f = F[fn]
    if f['kind'] not in ('root', 'leaf-no-pdata'):
        return False
    code = im.img[fn:fn + min(f['size'], 96)]
    return b'\xf6\xc2\x01' in code or b'\x80\xe2\x01' in code or b'\x83\xe2\x01' in code


pickle.dump(dict(classes=classes, tdname=tdname, direct=dict(direct), depth=depth, anc=anc, prim=prim, slot_info=slot_info,
                 users=dict(users), thunk_of=thunk_of, stores=dict(stores), vt2td=vt2td, PURECALL=PURECALL,
                 fam=dict(resolved=fam_by_td_slot, nonsynthetic=fam_nonsynthetic, synthetic=fam_synthetic, logical=fam_logical)),
            open(HERE + 'classes.pkl', 'wb'))
print('types', len(classes), 'with vtables', sum(1 for c in classes.values() if c['vtables']),
      'logical (with vtables)', len({c['logical'] for c in classes.values() if c['vtables']}),
      'logical (all)', len({c['logical'] for c in classes.values()}))
print('families (root td, slot):', fam_by_td_slot, 'nonsynthetic-root', fam_nonsynthetic, 'synthetic-root', fam_synthetic, 'logical roots', fam_logical)
print('cat', collections.Counter(v['cat'] for v in slot_info.values()))
print('thunks', len(thunk_of), collections.Counter(t['kind'] for t in thunk_of.values()), 'with target', sum(1 for t in thunk_of.values() if t['target']))
print('vptr stores in', len(stores), 'functions', sum(len(v) for v in stores.values()))
print('chain_virtual vtables', sum(1 for c in classes.values() for v in c['vtables'] if v['chain_virtual']))
