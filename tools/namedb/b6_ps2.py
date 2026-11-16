#!/usr/bin/env python3
"""B3 step 3d: PS2 decomp (GovanifY/kh2, MIT) -> PC name transfer WITHOUT byte patterns.
Evidence classes: class names (RTTI vs PS2 scope), file layout order (monotone alignment between anchors), string constants,
float/immediate constant tables, call-graph shape (callee counts, callee-pair consistency). PC side is rebuilt on the corrected
universe with merged extents and data-in-code excluded (S7 used first-fragment-only extents for 3,315 roots).
Output ps2v2.pkl: pairs with evidence + holdout precision per confidence bucket."""
import sys, pickle, collections, re, json, math, bisect, random, struct
sys.path.insert(0, '/home/user/projects/kh2-nmc-analysis/scratch/namedb2')
from lib import *
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP

im = Img()
img = im.img
U = load_universe()
F = {f['start_rva']: f for f in U}
B = pickle.load(open(HERE + 'cands.pkl', 'rb'))
SM = pickle.load(open(HERE + 'strmap.pkl', 'rb'))
STR = pickle.load(open(HERE + 'strings.pkl', 'rb'))
C = pickle.load(open(HERE + 'classes.pkl', 'rb'))
M = pickle.load(open(S7 + 'ps2match.pkl', 'rb'))
PX = pickle.load(open(S7 + 'ps2xref.pkl', 'rb'))
K = pickle.load(open(S7 + 'consts.pkl', 'rb'))
pfn, pstr = PX['fn'], PX['strs']
rows = json.load(open(S7 + 'ps2_names.json'))['rows']
pnamed = {r['addr']: r for r in rows if r['kind'] == 'named' and not re.match(r'^(ctarget_|wtarget_|u_tail|D_|func_)', r['dem'] or '')}
cmap = M['cmap']       # PC logical class -> PS2 scope (S7: 273 exact + 18 unique-leaf = 291)
inv_cmap = collections.defaultdict(list)
for lg, v in cmap.items():
    inv_cmap[v['ps2']].append(lg)

# ---- PC constants with merged extents
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
md.skipdata = True
FLT = {'movss', 'mulss', 'addss', 'subss', 'divss', 'comiss', 'ucomiss', 'maxss', 'minss', 'cvtss2sd', 'sqrtss', 'andps', 'andnps', 'orps', 'xorps', 'movaps', 'movups', 'mulps', 'addps', 'subps', 'movdqa', 'movd'}
pcc = {}
for r, f in F.items():
    if f['kind'] not in ('root', 'leaf-no-pdata'):
        continue
    bad = [(d[0], d[1]) for d in f['data_in_code']] + [(u[0], u[1]) for u in f['unreached_ranges'] if u[2] != 'unreached-code']
    cs = set()
    for a, b in f['extents']:
        cur = a
        segs = []
        for (x, y) in sorted(q for q in bad if q[0] < b and q[1] > a):
            if x > cur:
                segs.append((cur, x))
            cur = max(cur, y)
        if cur < b:
            segs.append((cur, b))
        for sa, sb in segs:
            for ins in md.disasm(img[sa:sb], sa):
                if ins.id == 0:
                    continue
                m = ins.mnemonic
                for op in ins.operands:
                    if op.type == X86_OP_IMM and m in ('mov', 'cmp', 'and', 'or', 'xor', 'add', 'sub', 'test', 'imul', 'push'):
                        v = op.imm & 0xffffffff
                        if 0x10000 <= v < 0xfffff000:
                            cs.add(v)
                    elif op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP and m in FLT:
                        t = ins.address + ins.size + op.mem.disp
                        if 0 <= t < im.size - 4:
                            v = struct.unpack_from('<I', img, t)[0]
                            if v != 0 and v != 0x80000000:
                                cs.add(v)
    pcc[r] = cs
print('pc consts computed', len(pcc))

# ---- per-function descriptors
thmap = {t: v['target'] for t, v in C['thunk_of'].items() if v['target']}


def pc_calls(r):
    out = []
    seen = set()
    for t in F[r]['calls']:
        t = thmap.get(t, t)
        if t in F and t not in seen and t != r:
            seen.add(t)
            out.append(t)
    return out


PC = [r for r, f in F.items() if f['kind'] in ('root', 'leaf-no-pdata') and f['size'] >= 8]
pcsz = {r: F[r]['size'] for r in PC}
pccall = {r: pc_calls(r) for r in PC}
pcstr = {}
for r in PC:
    ss = set()
    for k in SM['fn_strs'].get(r, ()):
        s = STR[k]
        if len(s) >= 4:
            ss.add(s)
    pcstr[r] = ss
PS = [a for a, v in pfn.items() if v['end'] - a >= 8]
pssz = {a: pfn[a]['end'] - a for a in PS}
pscall = {a: [t for t in dict.fromkeys(pfn[a]['calls']) if t in pfn and t != a] for a in PS}
psstr = {a: {pstr[s] for s in pfn[a]['strs'] if len(pstr[s]) >= 4} for a in PS}
psc = K['ps2']

# ---- anchors
anchors = {}       # p -> (q, how, score)


def add_anchor(p, q, how, sc):
    if p in anchors or q in {v[0] for v in anchors.values()}:
        return False
    anchors[p] = (q, how, sc)
    return True


# string anchors
pcref = collections.defaultdict(set)
psref = collections.defaultdict(set)
for r in PC:
    for s in pcstr[r]:
        pcref[s].add(r)
for a in PS:
    for s in psstr[a]:
        psref[s].add(a)
score = collections.Counter()
for s in set(pcref) & set(psref):
    w = 1.0 / (len(pcref[s]) * len(psref[s]))
    for p in psref[s]:
        for q in pcref[s]:
            score[(p, q)] += w
bp, bq = {}, {}
for (p, q), sc in score.items():
    if sc > bp.get(p, (0, None))[0]:
        bp[p] = (sc, q)
    if sc > bq.get(q, (0, None))[0]:
        bq[q] = (sc, p)
n_str = 0
used_q = set()
for (p, q), sc in sorted(score.items(), key=lambda kv: -kv[1]):
    if sc >= 0.99 and bp[p][1] == q and bq[q][1] == p:
        r_ = pssz[p] / max(pcsz[q], 1) if p in pssz else 1
        if add_anchor(p, q, 'string-anchor', sc):
            n_str += 1
# constant fingerprints
ip = collections.defaultdict(set)
ic = collections.defaultdict(set)
for a in PS:
    for v in psc.get(a, ()):
        ip[v].add(a)
for r in PC:
    for v in pcc.get(r, ()):
        ic[v].add(r)
cs_, cn = collections.Counter(), collections.Counter()
for v in set(ip) & set(ic):
    if len(ip[v]) <= 4 and len(ic[v]) <= 4:
        w = 1.0 / (len(ip[v]) * len(ic[v]))
        for p in ip[v]:
            for q in ic[v]:
                cs_[(p, q)] += w
                cn[(p, q)] += 1
bp2, bq2 = {}, {}
for (p, q), sc in cs_.items():
    if sc > bp2.get(p, (0, None))[0]:
        bp2[p] = (sc, q)
    if sc > bq2.get(q, (0, None))[0]:
        bq2[q] = (sc, p)
n_const = 0
for (p, q), sc in sorted(cs_.items(), key=lambda kv: -kv[1]):
    if cn[(p, q)] >= 2 and bp2[p][1] == q and bq2[q][1] == p and sc >= 1.0:
        if 0.25 <= pcsz[q] / max(pssz[p], 1) <= 6:
            if add_anchor(p, q, 'const-fingerprint(%d)' % cn[(p, q)], sc):
                n_const += 1
# constant-TABLE anchors: identical numeric table content referenced by one function on each side (data, not code bytes)
g_ps2 = open('/home/user/projects/kh2-decomp/SLPM_666.75', 'rb').read()
SC_ = pickle.load(open(HERE + 'scan.pkl', 'rb'))


def ps2rd(a, n):
    o = 0x1000 + a - 0x100000
    return g_ps2[o:o + n] if 0 <= o and o + n <= len(g_ps2) else None


def tab_ok(b, nd):
    if b is None or len(set(b)) < nd:
        return False
    return sum(1 for c in b if 0x20 <= c < 0x7f) < len(b) - 6


tab_stats = {}
pc_tab_sets = {}
for N_, nd, lim in ((32, 6, 3), (16, 5, 2)):
    pst = collections.defaultdict(set)
    for a in PS:
        for x in set(pfn[a]['imm']):
            if 0x367418 <= x < 0x37a4b4:
                b = ps2rd(x, N_)
                if tab_ok(b, nd):
                    pst[b].add(a)
    pct = collections.defaultdict(set)
    for r in PC:
        for (addr, t, mn) in SC_[r]['refs']:
            if mn != 'call' and im.sec(t) in ('.rdata', '.data') and t + N_ < im.size:
                b = img[t:t + N_]
                if tab_ok(b, nd):
                    pct[b].add(r)
    cand = []
    for b in set(pst) & set(pct):
        if len(pst[b]) <= lim and len(pct[b]) <= lim:
            for p in pst[b]:
                for q in pct[b]:
                    if 0.2 <= pcsz[q] / max(pssz[p], 1) <= 8:
                        cand.append((p, q, 1.0 / (len(pst[b]) * len(pct[b])), b))
    sc2 = collections.Counter()
    for p, q, w, b in cand:
        sc2[(p, q)] += w
    bpp, bqq = {}, {}
    for (p, q), w in sc2.items():
        if w > bpp.get(p, (0, None))[0]:
            bpp[p] = (w, q)
        if w > bqq.get(q, (0, None))[0]:
            bqq[q] = (w, p)
    nt = 0
    for (p, q), w in sorted(sc2.items(), key=lambda kv: -kv[1]):
        if bpp[p][1] == q and bqq[q][1] == p and w >= (0.99 if N_ == 32 else 0.99):
            if add_anchor(p, q, 'table-content(%d)' % N_, w):
                nt += 1
    tab_stats[N_] = dict(ps2_tables=len(pst), pc_tables=len(pct), common=len(set(pst) & set(pct)), anchors=nt)
print('table anchors', tab_stats)
n_tab = sum(v['anchors'] for v in tab_stats.values())

# ctor/dtor class anchors: PS2 'K::K' / 'K::~K' <-> the unique PC ctor/dtor (v2 classification) of the matched class
td_by_logical = collections.defaultdict(list)
for td, c in C['classes'].items():
    if c['vtables']:
        td_by_logical[c['logical']].append(td)
n_cd = 0
for a, r in pnamed.items():
    cl, me = r['class'], r['method']
    if not cl or cl not in inv_cmap:
        continue
    for lg in inv_cmap[cl]:
        tds = [t for t in td_by_logical.get(lg, []) if not C['classes'][t]['iface']]
        for td in tds:
            lst = B['ctor_rvas'].get(td, []) if me == cl.split('::')[-1] else (B['dtor_rvas'].get(td, []) if me == '~' + cl.split('::')[-1] else [])
            if len(lst) == 1:
                q = lst[0]
                if a in pssz and q in pcsz and 0.2 <= pcsz[q] / max(pssz[a], 1) <= 8:
                    if add_anchor(a, q, 'class-ctor' if not me.startswith('~') else 'class-dtor', 1.0):
                        n_cd += 1
print('anchors: string', n_str, 'const', n_const, 'class ctor/dtor', n_cd, 'total', len(anchors))

# ---- local call-graph propagation from anchors (layout order is NOT globally monotone between the two builds:
#      only 24 of 70 anchors form a monotone chain, a global alignment had 8% holdout precision, so it is not used for names)
def jacc(a, b):
    return len(a & b) / len(a | b) if (a or b) else 0.0


KSCALE = 0.98
pc_callers = collections.defaultdict(set)
for r in PC:
    for t in pccall[r]:
        pc_callers[t].add(r)
ps_callers = collections.defaultdict(set)
for a in PS:
    for t in pscall[a]:
        ps_callers[t].add(a)
UBP = {t for t, s_ in ps_callers.items() if len(s_) > 60}
UBC = {t for t, s_ in pc_callers.items() if len(s_) > 60}


def sim(p, q):
    s = -abs(math.log(max(pcsz[q], 1) / (KSCALE * pssz[p])))
    s -= 0.12 * min(abs(len(pccall[q]) - len(pscall[p])), 8)
    cp, cq = psc.get(p, set()), pcc.get(q, set())
    if cp or cq:
        s += 1.5 * jacc(cp, cq)
    if psstr[p] and pcstr[q]:
        s += 1.5 * jacc(psstr[p], pcstr[q])
    return s


def propagate(seed_pairs, minsim=-0.35, margin=0.35, rounds=8):
    """seed_pairs: dict p->q. Returns dict p->(q, sim, round, via)"""
    got = {p: (q, 9.9, 0, 'seed') for p, q in seed_pairs.items()}
    inv = {v[0]: p for p, v in got.items()}
    for rd in range(1, rounds + 1):
        new = {}
        newq = {}
        for p, (q, _s, _r, _v) in list(got.items()):
            for direction in ('callee', 'caller'):
                if direction == 'callee':
                    Pc = [x for x in pscall[p] if x not in got and x not in UBP and x in pssz]
                    Qc = [x for x in pccall[q] if x not in inv and x not in UBC and x in pcsz]
                else:
                    Pc = [x for x in ps_callers[p] if x not in got and x in pssz and len(ps_callers[p]) <= 6]
                    Qc = [x for x in pc_callers[q] if x not in inv and x in pcsz and len(pc_callers[q]) <= 6]
                if not Pc or not Qc or len(Pc) > 12 or len(Qc) > 12:
                    continue
                for a in Pc:
                    sc = sorted(((sim(a, b), b) for b in Qc), reverse=True)
                    if not sc or sc[0][0] < minsim:
                        continue
                    if len(sc) > 1 and sc[0][0] - sc[1][0] < margin:
                        continue
                    b = sc[0][1]
                    # reverse uniqueness: a must be b's best among Pc
                    rv = sorted(((sim(x, b), x) for x in Pc), reverse=True)
                    if rv[0][1] != a or (len(rv) > 1 and rv[0][0] - rv[1][0] < margin):
                        continue
                    if a in new and new[a][0] != b:
                        new[a] = (None, 0, rd, 'conflict')
                        continue
                    new[a] = (b, sc[0][0], rd, direction)
        ok = {a: v for a, v in new.items() if v[0] is not None}
        cnt = collections.Counter(v[0] for v in ok.values())
        ok = {a: v for a, v in ok.items() if cnt[v[0]] == 1 and v[0] not in inv}
        if not ok:
            break
        for a, v in ok.items():
            got[a] = v
            inv[v[0]] = a
    return got


anch_pairs = {p: v[0] for p, v in anchors.items() if p in pssz and v[0] in pcsz}
res = propagate(anch_pairs)
print('propagation from', len(anch_pairs), 'anchors ->', len(res), 'pairs (incl. seeds)')
# holdout: hide a quarter of the anchors, propagate from the rest, check the hidden ones that get re-derived
random.seed(11)
hold_stats = collections.defaultdict(lambda: [0, 0])
items = sorted(anch_pairs.items())
for trial in range(10):
    hold = set(random.sample([p for p, q in items], max(1, len(items) // 4)))
    use = {p: q for p, q in items if p not in hold}
    o = propagate(use)
    for p in hold:
        if p in o:
            hold_stats['predicted'][1] += 1
            hold_stats['predicted'][0] += (o[p][0] == anch_pairs[p])
        else:
            hold_stats['not-predicted'][1] += 1
    # also precision of ALL non-seed predictions against the anchors they touch (anchors outside use act as ground truth)
    for p, v in o.items():
        if p in hold:
            continue
# precision on held-out pairs only (ground truth); report the numbers
hold_stats = {k_: dict(correct=v[0], total=v[1]) for k_, v in hold_stats.items()}
print('holdout', hold_stats)
pairs = {}
for p, (q, s, rd, via) in res.items():
    if via == 'seed':
        h = anchors[p][1]
        pairs[p] = dict(q=q, how=h, score=anchors[p][2], conf='medium', rounds=0)
    else:
        pairs[p] = dict(q=q, how='callgraph-propagation(%s,r%d)' % (via, rd), score=s, conf='low', rounds=rd)
# ---- call-graph shape consistency: a pair is confirmed when its mapped callees agree with other pairs
q_of = {p: v['q'] for p, v in pairs.items()}
for p, v in pairs.items():
    q = v['q']
    cp = [q_of[x] for x in pscall.get(p, []) if x in q_of]
    cq = set(pccall.get(q, []))
    agree = sum(1 for x in cp if x in cq)
    v['callee_agree'] = (agree, len(cp))
    # class agreement
    pn = pnamed.get(p)
    qcl = None
    for c in B['cands'].get(q, []):
        if c['kind'] in ('virtual', 'ctor', 'dtor', 'deleting-dtor', 'ctor_or_dtor') and c['cls']:
            qcl = c['cls']
            break
    v['pc_class'] = qcl
    if pn and qcl and pn['class']:
        ok = qcl in inv_cmap.get(pn['class'], []) or cmap.get(qcl, {}).get('ps2') == pn['class']
        v['class_agree'] = bool(ok)
    else:
        v['class_agree'] = None
    # confidence
    strong = v['how'].split('(')[0] in ('string-anchor', 'const-fingerprint', 'class-ctor', 'class-dtor', 'table-content')
    nev = 0
    if strong:
        nev += 1
    if agree >= 1 and agree == len(cp):
        nev += 1
    if v['class_agree']:
        nev += 1
    if v['class_agree'] is False:
        v['conf'] = None          # class disagreement: reject
        continue
    if v['how'].startswith('callgraph-propagation'):
        v['conf'] = 'medium' if (v['class_agree'] and v['score'] > 0.3) else 'low'
    else:
        v['conf'] = 'high' if nev >= 2 else 'medium'
agree_stats = collections.Counter((v['how'].split('(')[0].split(',')[0], v['class_agree']) for v in pairs.values())
print('class-agreement check (True=PC class label agrees with PS2 class, False=rejected, None=no label on one side):', dict(agree_stats))
pairs = {p: v for p, v in pairs.items() if v['conf']}
named = {p: v for p, v in pairs.items() if p in pnamed}
cnt = collections.Counter((v['how'].split('(')[0], v['conf']) for v in named.values())
print('pairs', len(pairs), 'with a PS2 name', len(named), dict(cnt))
# per matched class: located (vtable known = always) and function-level transfers
cls_loc = {}
for lg, v in cmap.items():
    tds = td_by_logical.get(lg, [])
    nvt = sum(len(C['classes'][t]['vtables']) for t in tds)
    cls_loc[lg] = dict(ps2=v['ps2'], how=v['how'], vtables=nvt, types=len(tds), transfers=[])
for p, v in named.items():
    cl = pnamed[p]['class']
    for lg in inv_cmap.get(cl, []):
        cls_loc[lg]['transfers'].append((p, v['q'], v['conf']))
pickle.dump(dict(pairs=pairs, pnamed={a: dict(dem=r['dem'], cls=r['class'], method=r['method']) for a, r in pnamed.items()}, hold=hold_stats, anchors={p: v for p, v in anchors.items()},
                 pcc=pcc, cls_loc=cls_loc, agree_stats={'%s/%s' % k: v for k, v in agree_stats.items()}, inv_cmap={k_: v for k_, v in inv_cmap.items()}, cmap=cmap, pcc_n=len(pcc), nanchor=dict(string=n_str, const=n_const, ctor_dtor=n_cd, table=n_tab, table_detail=tab_stats)),
            open(HERE + 'ps2v2.pkl', 'wb'))
print('classes with >=1 transfer', sum(1 for v in cls_loc.values() if v['transfers']), 'of', len(cls_loc))
