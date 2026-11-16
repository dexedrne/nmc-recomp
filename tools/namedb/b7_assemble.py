#!/usr/bin/env python3
"""B3 step 4: assemble name_db_v2: choose the best name per function, vote classes through the call graph, funclet/thunk naming,
subsystem map (seeds -> adjacency -> call-graph), PS2 transfers validated against the final class labels, class_db_v2.
Outputs: db.pkl (everything), then b8_emit.py writes csv/json/reports."""
import sys, pickle, collections, re, json, bisect
sys.path.insert(0, '/home/user/projects/kh2-nmc-analysis/scratch/namedb2')
from lib import *

im = Img()
U = load_universe()
F = {f['start_rva']: f for f in U}
B = pickle.load(open(HERE + 'cands.pkl', 'rb'))
C = pickle.load(open(HERE + 'classes.pkl', 'rb'))
PS = pickle.load(open(HERE + 'ps2v2.pkl', 'rb'))
SM = pickle.load(open(HERE + 'strmap.pkl', 'rb'))
S7R = B['S7']
cands = collections.defaultdict(list, B['cands'])
callers_all = {k: set(v) for k, v in B['callers'].items()}
callees_all = {k: set(v) for k, v in B['callees'].items()}
classes = C['classes']

# ---------------------------------------------------------------- PS2 candidates (class-validated after the RTTI labels are known)
RANK_SRC = {'panacea-openkh': 0, 'pe-export-table': 1, 'clr-metadata-MethodDef': 2, 'assert-string-fn-name': 3, 'docs-spec': 4, 'platform-seams-proposal': 5,
            'docs-spec-gx-boundary': 6}
CONF = {'high': 3, 'medium': 2, 'low': 1}


def src_rank(c):
    s = c['source']
    if s in RANK_SRC:
        return RANK_SRC[s]
    if s.startswith('s7:'):
        return 7
    if s.startswith('ps2-decomp'):
        return 8
    if s == 'pe-imports':
        return 3
    return 20


def best(clist):
    real = [c for c in clist if c['tier'] == 'A-real-name']
    pool = real if real else clist
    return sorted(pool, key=lambda c: (src_rank(c), -CONF[c['conf']]))


# provisional class labels from RTTI/real-name candidates (independent of PS2) for the PS2 class-agreement check
def cls_of_cands(r):
    for c in cands.get(r, []):
        if c['kind'] in ('virtual', 'ctor', 'dtor', 'deleting-dtor', 'ctor_or_dtor', 'virtual-thunk', 'virtual-impl') and c['cls']:
            return c['cls']
    return None


pnamed = PS['pnamed']
inv_cmap = PS['inv_cmap']
cmap = PS['cmap']
ps2_stats = collections.Counter()
ps2_added = {}
for p, v in PS['pairs'].items():
    if p not in pnamed:
        continue
    q = v['q']
    pn = pnamed[p]
    qcl = cls_of_cands(q)
    conf = v['conf']
    agree = None
    if qcl and pn['cls']:
        agree = (cmap.get(qcl, {}).get('ps2') == pn['cls']) or (qcl in inv_cmap.get(pn['cls'], []))
        if not agree:
            ps2_stats['rejected_class_disagree'] += 1
            continue
        if conf != 'high':
            conf = 'high' if v['how'].split('(')[0] in ('string-anchor', 'const-fingerprint', 'table-content') else 'medium'
        ps2_stats['class_agree_confirmed'] += 1
    nm = (pn['cls'] + '::' + pn['method']) if pn['cls'] else pn['method']
    ps2_added[q] = dict(p=p, how=v['how'], conf=conf, name=nm)
    cands[q].append(dict(name=nm, cls=pn['cls'] or None, tier='A-real-name', conf=conf, source='ps2-decomp:' + v['how'].split('(')[0].split(',')[0],
                         evidence='PS2 decomp %08x %s <-> PC via %s (score %.2f, callee agreement %s, class agreement %s)' % (p, pn['dem'][:90], v['how'], v['score'], v.get('callee_agree'), agree),
                         sub=None, kind='ps2-transfer'))
ps2_stats['named_transfers'] = len(ps2_added)

# ---------------------------------------------------------------- choose names
row = {}
alts_n = collections.Counter()
conflicts = []
for r, f in F.items():
    row[r] = dict(rva=r, name=None, cls=None, tier=None, conf=None, source=None, evidence=None, sub=None, sub_basis=None, kind_name=None, alts=[], votes=None)
for r, cl in cands.items():
    if r not in F:
        continue
    ch = best(cl)
    b = ch[0]
    x = row[r]
    x.update(name=b['name'], cls=b['cls'], tier=b['tier'], conf=b['conf'], source=b['source'], evidence=b['evidence'], kind_name=b['kind'])
    if b.get('sub'):
        x['sub_hint'] = b['sub']
    x['alts'] = [(c['name'], c['source'], c['conf']) for c in ch[1:5] if c['name'] != b['name']]
    reals = {c['name'] for c in cl if c['tier'] == 'A-real-name' and c['kind'] not in ('s7',) and not c['name'].startswith('imp::')}
    if len(reals) > 1:
        conflicts.append((hex(r), sorted(reals)[:4]))
# the class of a function = class attribute of the winning candidate; if a real name won without class, fall back to the best role candidate's class
for r, cl in cands.items():
    x = row[r]
    if x['cls'] is None:
        for c in sorted(cl, key=lambda c: -CONF[c['conf']]):
            if c['cls'] and c['kind'] in ('virtual', 'ctor', 'dtor', 'deleting-dtor', 'virtual-thunk', 'virtual-impl'):
                x['cls_role'] = c['cls']
                break

# ---------------------------------------------------------------- funclets and thunks named from their parents / targets
th_target = {t: v['target'] for t, v in C['thunk_of'].items() if v['target']}


def nm_of(r):
    return row[r]['name'] if r in row else None


nfun = nthk = 0
for r, f in F.items():
    x = row[r]
    if x['name']:
        continue
    if f['kind'] == 'funclet':
        ps = [p for p in f['funclet_parents'] if p in row and row[p]['name']]
        if ps:
            P0 = ps[0]
            x.update(name='%s$%s_%x' % (row[P0]['name'], f['funclet_kind'].replace('-', '_'), r), cls=row[P0]['cls'] or row[P0].get('cls_role'), tier='B-class-role', conf='high', source='eh-funclet-parent',
                     evidence='%s funclet of %s named by its EH table%s' % (f['funclet_kind'], row[P0]['name'], '' if len(f['funclet_parents']) == 1 else ' (+%d other parents)' % (len(f['funclet_parents']) - 1)), kind_name='funclet')
            nfun += 1
    elif f['kind'] in ('thunk', 'trampoline') and not f['imports']:
        tg = th_target.get(r) or (f['tail_targets'][0] if len(f['tail_targets']) == 1 else None)
        if tg and tg in row and row[tg]['name'] and not row[tg]['name'].startswith('imp::') or (tg and tg in row and row[tg]['name']):
            x.update(name='thunk_to_%s' % row[tg]['name'], cls=row[tg]['cls'], tier='B-class-role', conf='medium', source='thunk-target',
                     evidence='%s whose only effect is a jump to %s' % (f['kind'], hex(tg)), kind_name='thunk')
            nthk += 1

# ---------------------------------------------------------------- class voting through the call graph
def known_class(r):
    x = row[r]
    return x['cls'] or x.get('cls_role')


keys_sorted = sorted(F)
kidx = {k: i for i, k in enumerate(keys_sorted)}


def region_class(r, maxgap=4, skip=()):
    i = kidx[r]
    L = R = None
    for j in range(i - 1, max(-1, i - 1 - maxgap), -1):
        k = keys_sorted[j]
        if F[k]['kind'] in ('funclet', 'import-stub'):
            continue
        c = known_class(k) if k not in skip else None
        if c:
            L = c
            break
    for j in range(i + 1, min(len(keys_sorted), i + 1 + maxgap)):
        k = keys_sorted[j]
        if F[k]['kind'] in ('funclet', 'import-stub'):
            continue
        c = known_class(k) if k not in skip else None
        if c:
            R = c
            break
    return L, R


def class_votes_round(rnd, minconf):
    new = {}
    for r, f in F.items():
        if r in row and (known_class(r) or row[r]['name']):
            continue
        if f['kind'] in ('import-stub',):
            continue
        cs = callers_all.get(r, set())
        # a funclet parent / caller chain: calls from funclets count as calls from their parents
        if len(cs) > 40:
            continue
        votes = collections.Counter()
        unk = 0
        for c in cs:
            if c == r:
                continue
            cc = known_class(c)
            if c in F and F[c]['kind'] == 'funclet':
                for p in F[c]['funclet_parents']:
                    cc = cc or known_class(p)
            if cc:
                votes[cc] += 1
            else:
                unk += 1
        tot = sum(votes.values())
        L, Rr = region_class(r)
        reg = L if (L and L == Rr) else None
        cvote = None
        if tot:
            cls0, n = votes.most_common(1)[0]
            if (len(votes) == 1 and unk == 0) or (n / tot >= 0.8 and tot >= 2 and n > unk):
                cvote = cls0
        if reg and cvote == reg:
            cls, conf = reg, ('medium' if rnd == 1 else 'low')
        elif reg and rnd <= 2:
            cls, conf = reg, ('medium' if rnd == 1 else 'low')
        elif cvote and rnd <= 3:
            cls, conf = cvote, 'low'
        else:
            continue
        if CONF[conf] < CONF[minconf]:
            continue
        new[r] = (cls, conf, dict(votes), unk, (reg, cvote))
    return new


nvote = 0
vote_rounds = []
for rnd in range(1, 5):
    new = class_votes_round(rnd, 'low')
    if not new:
        break
    vote_rounds.append(len(new))
    for r, (cls, conf, votes, unk, rv_) in new.items():
        x = row[r]
        x['cls_voted'] = cls
        x['votes'] = dict(votes=votes, unknown_callers=unk, round=rnd, region_class=rv_[0], caller_vote_class=rv_[1])
        x['cls_role'] = cls
        if not x['name']:
            x.update(name='%s::fn_%x' % (cls, r), cls=cls, tier='C-class-vote', conf=conf, source='callgraph-class-vote' if (rv_[1] == cls) else 'class-region-adjacency',
                     evidence='%s%s%s (round %d)' % ('both neighbours within 4 functions are %s; ' % rv_[0] if rv_[0] else '', ('callers: ' + ', '.join('%d x %s' % (n, c) for c, n in votes.items()) + (', %d callers unlabelled' % unk if unk else ', all callers labelled')) if votes else 'no labelled callers', '' if rv_[1] != cls else ' [caller vote agrees]', rnd), kind_name='class-vote')
        nvote += 1
# ---------------------------------------------------------------- holdout precision of the class vote (hide 20% of RTTI-labelled non-virtual-style functions)
import random as _rnd
_rnd.seed(9)
known_fn = [r for r in row if row[r]['source'] in ('rtti+vptr-store', 'rtti-vtable') and (row[r]['cls'] or row[r].get('cls_role')) and F[r]['kind'] in ('root', 'leaf-no-pdata')]
hold = set(_rnd.sample(known_fn, len(known_fn) // 5))
hv = collections.defaultdict(lambda: [0, 0])
for r in hold:
    votes = collections.Counter()
    unk = 0
    for c in callers_all.get(r, ()):
        if c == r:
            continue
        cc = None if c in hold else known_class(c)
        if cc:
            votes[cc] += 1
        else:
            unk += 1
    tot = sum(votes.values())
    if not tot:
        hv['no_labelled_callers'][1] += 1
        continue
    cls_, n = votes.most_common(1)[0]
    truth = row[r]['cls'] or row[r].get('cls_role')
    if len(votes) == 1 and unk == 0:
        k_ = 'all callers labelled, one class'
    elif n / tot >= 0.8 and tot >= 2 and n > unk:
        k_ = '>=80% of >=2 labelled callers'
    else:
        hv['not_predicted'][1] += 1
        continue
    hv[k_][1] += 1
    hv[k_][0] += (cls_ == truth)
vote_holdout = {k_: dict(correct=v[0], predicted=v[1]) for k_, v in hv.items()}
# second, applicable check: real-named non-virtual functions whose class comes from the NAME (string/docs/PS2/seam names), vote computed ignoring their own label
real_cls = [r for r in row if row[r]['tier'] == 'A-real-name' and row[r]['cls'] and row[r]['source'] and (row[r]['source'].startswith(('assert-string', 'ps2-decomp', 'docs-spec', 'platform-seams')))
            and F[r]['kind'] in ('root', 'leaf-no-pdata') and r not in C['users']]
hv2 = collections.defaultdict(lambda: [0, 0])
for r in real_cls:
    votes = collections.Counter()
    unk = 0
    for c in callers_all.get(r, ()):
        if c == r:
            continue
        cc = known_class(c) if c not in real_cls else None
        if cc:
            votes[cc] += 1
        else:
            unk += 1
    tot = sum(votes.values())
    if not tot:
        hv2['no_labelled_callers'][1] += 1
        continue
    cls_, n = votes.most_common(1)[0]
    truth = row[r]['cls']
    ok_ = (cls_ == truth) or (cmap.get(cls_, {}).get('ps2') == truth) or truth in inv_cmap and cls_ in inv_cmap[truth]
    if len(votes) == 1 and unk == 0:
        k_ = 'all callers labelled, one class'
    elif n / tot >= 0.8 and tot >= 2 and n > unk:
        k_ = '>=80% of >=2 labelled callers'
    else:
        hv2['not_predicted'][1] += 1
        continue
    hv2[k_][1] += 1
    hv2[k_][0] += bool(ok_)
# adjacency (class region) vs name-derived class
rg = collections.defaultdict(lambda: [0, 0])
skipset = set(real_cls)
for r in real_cls:
    L, R = region_class(r, 4, skipset)
    if L and L == R:
        truth = row[r]['cls']
        ok_ = (L == truth) or (cmap.get(L, {}).get('ps2') == truth) or (truth in inv_cmap and L in inv_cmap[truth])
        rg['region(both neighbours same class, gap<=4)'][1] += 1
        rg['region(both neighbours same class, gap<=4)'][0] += bool(ok_)
        # combined with callers
        votes = collections.Counter(known_class(c) for c in callers_all.get(r, ()) if c != r and c not in skipset and known_class(c))
        if votes and votes.most_common(1)[0][0] == L:
            rg['region AND callers agree'][1] += 1
            rg['region AND callers agree'][0] += bool(ok_)
vote_holdout['region_adjacency_vs_name_class'] = {k_: dict(correct=v[0], predicted=v[1]) for k_, v in rg.items()}
vote_holdout['name_labelled_nonvirtual_functions'] = {k_: dict(correct=v[0], predicted=v[1]) for k_, v in hv2.items()}


# ---------------------------------------------------------------- PS2 class-scoped assignment (after RTTI labels + votes)
import math, random
PX = pickle.load(open(S7 + 'ps2xref.pkl', 'rb'))
K7 = pickle.load(open(S7 + 'consts.pkl', 'rb'))
pfn, pstr = PX['fn'], PX['strs']
psc = K7['ps2']
pcc = PS['pcc']
SMS = pickle.load(open(HERE + 'strings.pkl', 'rb'))
ps_sz = {a: v['end'] - a for a, v in pfn.items()}
ps_calls = {a: [t for t in dict.fromkeys(v['calls']) if t in pfn and t != a] for a, v in pfn.items()}
ps_str = {a: {pstr[x] for x in v['strs'] if len(pstr[x]) >= 4} for a, v in pfn.items()}
pc_str = {}
for r in F:
    pc_str[r] = {SMS[k] for k in SM['fn_strs'].get(r, ()) if len(SMS[k]) >= 4}
pc_calls = {r: [th_target.get(t, t) for t in dict.fromkeys(F[r]['calls'])] for r in F}
KS = 0.98


def jac(a, b):
    return len(a & b) / len(a | b) if (a or b) else 0.0


def sim2(p, q):
    s = -abs(math.log(max(F[q]['size'], 1) / (KS * max(ps_sz[p], 1))))
    s -= 0.12 * min(abs(len(pc_calls[q]) - len(ps_calls[p])), 8)
    cp, cq = psc.get(p, set()), pcc.get(q, set())
    if cp or cq:
        s += 1.5 * jac(cp, cq)
    if ps_str[p] or pc_str[q]:
        s += 1.5 * jac(ps_str[p], pc_str[q])
    return s


ps_methods = collections.defaultdict(list)       # PS2 class -> [p]
for p, pn in pnamed.items():
    if pn['cls'] and p in ps_sz and ps_sz[p] >= 16:
        ps_methods[pn['cls']].append(p)
pc_members = collections.defaultdict(list)       # PC logical class -> [q]
for r, x in row.items():
    k = known_class(r)
    if k and F[r]['kind'] in ('root', 'leaf-no-pdata') and F[r]['size'] >= 16:
        pc_members[k].append(r)
already_q = {v['q'] for v in ps2_added.values()} if False else set(ps2_added)
already_p = {v['p'] for v in ps2_added.values()}


def assign(Ps, Qs, minsim=-0.30, margin=0.40):
    out = {}
    S = {(p, q): sim2(p, q) for p in Ps for q in Qs}
    for p in Ps:
        sc = sorted(((S[(p, q)], q) for q in Qs), reverse=True)
        if not sc or sc[0][0] < minsim:
            continue
        if len(sc) > 1 and sc[0][0] - sc[1][0] < margin:
            continue
        q = sc[0][1]
        rv = sorted(((S[(pp, q)], pp) for pp in Ps), reverse=True)
        if rv[0][1] != p or (len(rv) > 1 and rv[0][0] - rv[1][0] < margin):
            continue
        out[p] = (q, sc[0][0])
    return out


cs_pairs = {}
for pcl, Ps in ps_methods.items():
    for lg in inv_cmap.get(pcl, []):
        Qs = [q for q in pc_members.get(lg, []) if not (row[q]['tier'] == 'A-real-name')]
        Ps2 = [p for p in Ps if p not in already_p]
        if not Ps2 or not Qs or len(Ps2) > 40 or len(Qs) > 60:
            continue
        for p, (q, s_) in assign(Ps2, Qs).items():
            cs_pairs[p] = (q, s_, lg)
# negative control: assign PS2 methods of class A against PC members of a *different* matched class B (same procedure)
random.seed(5)
lgs = [lg for lg in pc_members if any(lg in inv_cmap.get(pcl, []) for pcl in ps_methods)]
neg_acc = neg_tot = 0
pcls_list = [pcl for pcl in ps_methods if any(inv_cmap.get(pcl, []))]
for pcl in pcls_list:
    Ps = [p for p in ps_methods[pcl] if p not in already_p]
    if not Ps or len(Ps) > 40:
        continue
    for _ in range(3):
        lg = random.choice(lgs)
        if lg in inv_cmap.get(pcl, []):
            continue
        Qs = [q for q in pc_members.get(lg, []) if row[q]['tier'] != 'A-real-name']
        if not Qs or len(Qs) > 60:
            continue
        neg_tot += len(Ps)
        neg_acc += len(assign(Ps, Qs))
pos_tot = sum(len([p for p in Ps if p not in already_p]) for pcl, Ps in ps_methods.items() if inv_cmap.get(pcl))
cs_stats = dict(accepted=len(cs_pairs), ps2_methods_considered=pos_tot, negative_control_accepted=neg_acc, negative_control_considered=neg_tot,
                negative_control_rate=(neg_acc / neg_tot if neg_tot else None), positive_rate=(len(cs_pairs) / pos_tot if pos_tot else None))
ncs = 0
use_cs = bool(cs_stats['negative_control_rate'] is not None and (cs_stats['positive_rate'] or 0) > 2.5 * cs_stats['negative_control_rate'])
cs_stats['applied'] = use_cs
cs_stats['reason'] = 'applied' if use_cs else 'NOT applied: acceptance on true class pairs (%.1f%%) is not clearly above the wrong-class negative control (%.1f%%), so the assignment has no discriminating power' % (100 * (cs_stats['positive_rate'] or 0), 100 * (cs_stats['negative_control_rate'] or 0))
for p, (q, s_, lg) in (cs_pairs.items() if use_cs else []):
    if q in ps2_added or row[q]['tier'] == 'A-real-name':
        continue
    pn = pnamed[p]
    conf = 'medium' if s_ >= 0.3 else 'low'
    nm = pn['cls'] + '::' + pn['method']
    row[q].update(name=nm, cls=pn['cls'], tier='A-real-name', conf=conf, source='ps2-decomp:class-scoped-assignment',
                  evidence='PS2 decomp %08x %s assigned within matched class %s (mutual best of %d candidates, sim %.2f; negative-control acceptance %.1f%%)' % (p, pn['dem'][:80], lg, len(pc_members.get(lg, [])), s_, 100 * (cs_stats['negative_control_rate'] or 0)),
                  kind_name='ps2-transfer')
    ps2_added[q] = dict(p=p, how='class-scoped-assignment', conf=conf, name=nm)
    ncs += 1
ps2_stats['class_scoped_named'] = ncs
ps2_stats['class_scoped'] = cs_stats

# re-run funclet/thunk naming now that more parents are named
for r, f in F.items():
    x = row[r]
    if x['name']:
        continue
    if f['kind'] == 'funclet':
        ps = [p for p in f['funclet_parents'] if p in row and row[p]['name']]
        if ps:
            P0 = ps[0]
            x.update(name='%s$%s_%x' % (row[P0]['name'], f['funclet_kind'].replace('-', '_'), r), cls=row[P0]['cls'] or row[P0].get('cls_role'), tier='B-class-role', conf='medium', source='eh-funclet-parent',
                     evidence='%s funclet of %s (parent named by a later pass)' % (f['funclet_kind'], row[P0]['name']), kind_name='funclet')
            nfun += 1
    elif f['kind'] in ('thunk', 'trampoline') and not f['imports']:
        tg = th_target.get(r) or (f['tail_targets'][0] if len(f['tail_targets']) == 1 else None)
        if tg and tg in row and row[tg]['name']:
            x.update(name='thunk_to_%s' % row[tg]['name'], cls=row[tg]['cls'] or row[tg].get('cls_role'), tier='B-class-role', conf='medium', source='thunk-target',
                     evidence='%s whose only effect is a jump to %s' % (f['kind'], hex(tg)), kind_name='thunk')
            nthk += 1

# ---------------------------------------------------------------- subsystem map
def class_sub(q):
    if not q:
        return None
    q = re.sub(r'^(class|struct) ', '', q)
    if q.startswith('Axa::GX::') or q.startswith('GX::') or q == 'GX':
        return 'gx-graphics'
    if q.startswith(('Axa::CFileMan', 'Axa::PackageFile', 'Axa::PackageMan', 'Axa::AxaResourceMan', 'Axa::RootFileData', 'Axa::DependencyFileData', 'Axa::ResourceEntry', 'Axa::ReplaceEntry', 'Axa::RequestQueue',
                     'Axa::CSJointListT<struct Axa::RootFileData', 'Axa::CSJointT<struct Axa::RootFileData')):
        return 'filemgr'
    if q.startswith(('Axa::JobManager', 'Axa::Job', 'Axa::Fiber')):
        return 'jobmgr-fibers'
    if q.startswith(('Axa::AxaSound', 'Axa::Sound::', 'Axa::BankEntry', 'Axa::PlaySoundStream', 'SQEX::', 'IXAudio2', 'IXAPO', 'CXAPO', 'ExternalVoice', 'SoundList', 'VAG_STREAM', 'Sd::')):
        return 'sd-audio'
    if q.startswith(('Axa::CTrophyMan', 'Axa::CStatsMan', 'Axa::TrophyUnlock', 'CCallback', 'CSteam', 'SteamPlatform', 'Steam::')) or 'Steam' in q:
        return 'steam'
    if q.startswith(('Axa::DebugPad', 'Axa::RumblingEntry', 'Axa::GamePad', 'Axa::DXInput')) or 'KEYBOARDDATA' in q:
        return 'input'
    if q.startswith(('Axa::CCipherMovie', 'Movie::')):
        return 'movie'
    if q.startswith(('Axa::PCSettings',)):
        return 'settingmenu'
    if q.startswith('Axa::'):
        return 'axa-platform'
    if q.startswith(('std::', 'ATL::', 'type_info', 'IUnknown')) or q.startswith('<lambda') or 'std::' in q.split('<')[0]:
        return 'crt-stl-lib'
    if q.startswith('SettingMenu'):
        return 'settingmenu'
    if q.startswith(('gb::', 'gm::', 'GUMMI')):
        return 'gummi'
    m = re.match(r'^(\w+)::', q)
    ns = m.group(1) if m else None
    nsmap = {'YS': 'kh2:yasui-YS', 'kn': 'kh2:naka-kn', 'dk': 'kh2:harata-dk', 'Tz': 'kh2:tozawa-menu-Tz', 'Ti': 'kh2:taito-minigame-Ti', 'Ts': 'kh2:sugi-action-Ts', 'YI': 'kh2:ishi-image-YI',
             'YW': 'kh2:libyw-widget-YW', 'ryj': 'kh2:rikeda-effect-ryj', 'sa': 'kh2:sasa-musical-sa', 'worldmap': 'kh2:worldmap', 'menu': 'kh2:menu', 'ppp': 'kh2:ppp-effects'}
    if ns in nsmap:
        return nsmap[ns]
    if q in ('GAME_MODE', 'TASK_MANAGER', 'AREA_OBJECT', 'KNAList', 'KNMList', 'MDLXList', 'findeList', 'inport_list', 'ALLOCATOR', 'GAME_SAVE', 'BOOT', 'FIELD_COMMAND'):
        return 'kh2:global-game'
    if q == 'CCallbackBase':
        return 'steam'
    return None


STRONG_IMP = {'d3d11.dll': 'gx-graphics', 'd3d12.dll': 'gx-graphics', 'dxgi.dll': 'gx-graphics', 'D3DCOMPILER_47.dll': 'gx-graphics', 'XAudio2_9.dll': 'sd-audio', 'XINPUT1_4.dll': 'input', 'DINPUT8.dll': 'input',
              'HID.DLL': 'input', 'SETUPAPI.dll': 'input', 'steam_api64.dll': 'steam', 'MFPlat.DLL': 'movie', 'MFReadWrite.dll': 'movie', 'mscoree.dll': 'clr-atl', 'WINMM.dll': 'sd-audio', 'dbghelp.dll': 'axa-platform',
              'imagehlp.dll': 'axa-platform', 'Mfplat.dll': 'movie', 'MF.dll': 'movie'}
FIBER_APIS = {'CreateFiber', 'DeleteFiber', 'SwitchToFiber', 'ConvertThreadToFiber', 'ConvertFiberToThread', 'CreateFiberEx', 'ConvertThreadToFiberEx'}
FILE_SUB = [(r'music2soundgit\\cdev\\sd', 'sd-audio'), (r'kingdom2\\kh2convert', 'axa-platform'), (r'kingdom2\\common', 'kh2:global-game'), (r'kingdom2\\gummi', 'gummi'), (r'kingdom2\\axa', 'axa-platform')]
SUBREASON = {}
lab = {}
rank_conf = {'high': 3, 'medium': 2, 'low': 1}


def setlab(r, sub, conf, why, basis='seed'):
    if not sub or r not in F:
        return
    if r not in lab or rank_conf[conf] > rank_conf[lab[r][1]]:
        lab[r] = (sub, conf, why, basis)


for r, f in F.items():
    x = row[r]
    # docs/panacea/seam hints carry their own subsystem
    for c in cands.get(r, []):
        if c.get('sub'):
            setlab(r, c['sub'], 'high' if c['conf'] != 'low' else 'medium', 'name-source subsystem hint (%s)' % c['source'])
    q = x['cls'] or x.get('cls_role')
    if q and (x['tier'] in ('A-real-name', 'B-class-role')):
        s = class_sub(q)
        setlab(r, s, 'high' if x['conf'] != 'low' else 'medium', 'class %s' % q)
    elif q and x['tier'] == 'C-class-vote' and x['conf'] == 'medium':
        setlab(r, class_sub(q), 'medium', 'class %s (region/caller evidence)' % q, 'seed-vote')
    nm = x['name'] or ''
    if nm.startswith('png_'):
        lab[r] = ('libpng', 'high', 'libpng export name', 'seed')
    elif f['sources'] and 'export' in f['sources'] and nm:
        if 'Steam' in nm or 'Presence' in nm or 'GameAppId' in nm:
            setlab(r, 'steam', 'high', 'steam export')
        elif nm.startswith('fnkh2'):
            setlab(r, 'axa-platform', 'high', 'kh2convert export')
        elif not nm.startswith('png_'):
            setlab(r, 'axa-platform', 'medium', 'export')
    if nm.startswith('imp::'):
        dll = f['imports'][0].split('!')[0] if f['imports'] else ''
        setlab(r, STRONG_IMP.get(dll, 'crt-stl-lib'), 'high', 'import stub of ' + dll)
    # imports used by the function
    if f['imports'] and f['kind'] in ('root', 'leaf-no-pdata'):
        cnt = collections.Counter()
        for im_ in f['imports']:
            dll, api = im_.split('!', 1)
            if api in FIBER_APIS:
                cnt['jobmgr-fibers'] += 3
            if dll in STRONG_IMP:
                cnt[STRONG_IMP[dll]] += 1
        if cnt:
            s, n = cnt.most_common(1)[0]
            setlab(r, s, 'medium', 'imports ' + ','.join(sorted(set(i.split('!')[0] for i in f['imports']))[:3]))
    for fs in SM['file_of'].get(r, []):
        for pat, sub in FILE_SUB:
            if re.search(pat, fs, re.I):
                setlab(r, sub, 'high', '__FILE__ string ' + fs.split('\\')[-1])
                break
    if r in SM['log_of']:
        setlab(r, 'sd-audio', 'medium', '[sd] log string')
# S7 seeds (basis seed) re-validated: kept when our own labelling does not contradict them
s7_seed_ok = s7_seed_conflict = s7_seed_new = 0
for r, v in S7R.items():
    if r not in F or v['subsystem'] is None or v['subsystem_basis'] != 'seed':
        continue
    if r in lab and lab[r][3] == 'seed':
        if lab[r][0] == v['subsystem']:
            s7_seed_ok += 1
        else:
            s7_seed_conflict += 1
    else:
        setlab(r, v['subsystem'], 'medium', 'S7 seed (class/name/string evidence), carried over', 'seed-s7')
        s7_seed_new += 1
# funclets inherit the subsystem of their parents; thunks of their targets (structural, high)
for r, f in F.items():
    if f['kind'] == 'funclet' and r not in lab:
        subs = collections.Counter(lab[p][0] for p in f['funclet_parents'] if p in lab)
        if subs and len(subs) == 1:
            setlab(r, subs.most_common(1)[0][0], 'medium', 'funclet of a labelled parent', 'structural')
    if f['kind'] in ('thunk', 'trampoline') and r not in lab:
        tg = th_target.get(r) or (f['tail_targets'][0] if len(f['tail_targets']) == 1 else None)
        if tg in lab:
            setlab(r, lab[tg][0], 'medium', 'thunk to a labelled function', 'structural')
keys = sorted(F)
idx = {k: i for i, k in enumerate(keys)}
callers_n = {k: len(v) for k, v in callers_all.items()}


def adjacency(lab_, maxgap=25):
    out = dict(lab_)
    L = [i for i, k in enumerate(keys) if k in lab_]
    for a, b in zip(L, L[1:]):
        if b - a > 1 and b - a - 1 <= maxgap and lab_[keys[a]][0] == lab_[keys[b]][0]:
            for i in range(a + 1, b):
                out.setdefault(keys[i], (lab_[keys[a]][0], 'low', 'same subsystem on both sides within %d functions' % maxgap, 'adjacency'))
    return out


def graph(lab_, rounds=3):
    lab_ = dict(lab_)
    for _ in range(rounds):
        new = {}
        for r in F:
            if r in lab_:
                continue
            v = collections.Counter()
            for t in callers_all.get(r, ()):
                if t in lab_:
                    v[lab_[t][0]] += 1
            for t in callees_all.get(r, ()):
                if t in lab_ and callers_n.get(t, 0) <= 200 and lab_[t][0] != 'crt-stl-lib':
                    v[lab_[t][0]] += 1
            tot = sum(v.values())
            if tot >= 2:
                s, n = v.most_common(1)[0]
                if n / tot >= 0.7:
                    new[r] = (s, 'low', 'call-graph vote %d/%d' % (n, tot), 'callgraph')
        if not new:
            break
        lab_.update(new)
    return lab_


seed_lab = dict(lab)
lab = adjacency(lab)
lab = graph(lab)
lab = adjacency(lab)
for r in F:
    if r in lab:
        row[r]['sub'], row[r]['sub_conf'], row[r]['sub_why'], row[r]['sub_basis'] = lab[r]
    else:
        row[r]['sub'] = None
# ---------------------------------------------------------------- finalize rows
final = []
for r in sorted(F):
    f = F[r]
    x = row[r]
    cls = x['cls'] or x.get('cls_role')
    vinfo = None
    us = C['users'].get(r)
    if us:
        vinfo = [dict(cls=classes[td]['logical'], vtable=hex(vt), slot=s, offset=off) for (td, vt, off, s) in us[:12]]
    final.append(dict(
        id=f['id'], rva=hx(r), va=hx(r + BASE), end=hx(f['extents'][-1][1]), size=f['size'], kind=f['kind'], has_pdata=f['has_pdata'], n_pdata_entries=f['n_pdata_entries'],
        funclet=(f['funclet_kind'] or ''), funclet_parents=[hx(p) for p in f['funclet_parents']], data_in_code_bytes=sum(d[1] - d[0] for d in f['data_in_code']),
        data_in_code=[[hx(d[0]), hx(d[1]), d[2]] for d in f['data_in_code']], extents=[[hx(a), hx(b)] for a, b in f['extents']], nranges=len(f['ranges']),
        universe_confidence=f['confidence'], cls=cls, name=x['name'], tier=x['tier'], confidence=x['conf'], source=x['source'], evidence=x['evidence'],
        subsystem=x['sub'], subsystem_basis=x['sub_basis'], subsystem_conf=x.get('sub_conf'), subsystem_why=x.get('sub_why'), name_kind=x['kind_name'],
        alts=x['alts'], class_votes=x['votes'], virtual=vinfo,
        s7=dict(name=S7R[r]['name'], cls=S7R[r]['class'], subsystem=S7R[r]['subsystem'], size=S7R[r]['size']) if r in S7R else None,
        n_callers=len(callers_all.get(r, ())), n_callees=len(callees_all.get(r, ()))))
pickle.dump(dict(final=final, ps2_stats=dict(ps2_stats), cs_stats=cs_stats, vote_holdout=vote_holdout, ps2_added={hex(k): v for k, v in ps2_added.items()}, conflicts=conflicts, vote_rounds=vote_rounds,
                 s7_seed=dict(ok=s7_seed_ok, conflict=s7_seed_conflict, new=s7_seed_new), nfun=nfun, nthk=nthk, seed_n=len(seed_lab)),
            open(HERE + 'db.pkl', 'wb'))
tiers = collections.Counter(x['tier'] for x in final)
print('rows', len(final), 'tiers', dict(tiers), 'with subsystem', sum(1 for x in final if x['subsystem']), 'class-labelled', sum(1 for x in final if x['cls']))
print('ps2', dict(ps2_stats), 'vote rounds', vote_rounds, 'funclets named', nfun, 'thunks named', nthk, 'name conflicts', len(conflicts))
print('vote holdout', vote_holdout)
print('s7 seed revalidation', s7_seed_ok, s7_seed_conflict, s7_seed_new)
