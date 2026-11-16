#!/usr/bin/env python3
"""B3 step 5: write name_db_v2.csv/json, class_db_v2.json, families.json, vtables_v2.csv, data_labels.csv, reconcile.json,
coverage.json/coverage.md, subsystems.json, worklist.json/worklist.md."""
import sys, pickle, collections, csv, json, re
sys.path.insert(0, '/home/user/projects/kh2-nmc-analysis/scratch/namedb2')
from lib import *

D = pickle.load(open(HERE + 'db.pkl', 'rb'))
C = pickle.load(open(HERE + 'classes.pkl', 'rb'))
R = pickle.load(open(HERE + 'rtti.pkl', 'rb'))
B = pickle.load(open(HERE + 'cands.pkl', 'rb'))
PS = pickle.load(open(HERE + 'ps2v2.pkl', 'rb'))
U = load_universe()
Fu = {f['start_rva']: f for f in U}
fin = D['final']
byrva = {int(x['rva'], 16): x for x in fin}
classes, tdname, slot_info, prim, users = C['classes'], C['tdname'], C['slot_info'], C['prim'], C['users']
OUT = HERE

# ------------------------------------------------------------ name_db_v2.csv / json
cols = ['id', 'rva', 'end', 'size', 'kind', 'has_pdata', 'funclet', 'data_in_code_bytes', 'class', 'name', 'subsystem', 'subsystem_basis', 'confidence', 'tier', 'source', 'evidence', 'virtual', 'n_callers', 'universe_confidence']
with open(OUT + 'name_db_v2.csv', 'w', newline='') as fh:
    w = csv.writer(fh)
    w.writerow(cols)
    for x in fin:
        v = ''
        if x['virtual']:
            v = ';'.join('%s#%d@%s+%d' % (q['cls'], q['slot'], q['vtable'], q['offset']) for q in x['virtual'][:4])
        w.writerow([x['id'], x['rva'], x['end'], x['size'], x['kind'], int(x['has_pdata']), x['funclet'], x['data_in_code_bytes'], x['cls'] or '', x['name'] or '', x['subsystem'] or '',
                    x['subsystem_basis'] or '', x['confidence'] or '', x['tier'] or '', x['source'] or '', (x['evidence'] or '')[:400], v, x['n_callers'], x['universe_confidence']])
json.dump(fin, open(OUT + 'name_db_v2.json', 'w'), separators=(',', ':'))

# ------------------------------------------------------------ class_db_v2.json
def fname(r):
    return byrva[r]['name'] if r in byrva else None


cdb = []
fam_members = collections.defaultdict(list)
for td, c in sorted(classes.items(), key=lambda kv: kv[1]['name']):
    allb = sorted(tdname[b] for b in C['anc'][td])
    vts = []
    n_slots = n_pure = 0
    all_named = True
    for v in c['vtables']:
        sl = []
        for s, t in enumerate(v['slots']):
            si = slot_info[(v['rva'], s)]
            nu = len(users[t])
            sl.append(dict(slot=s, fn=hx(t), name=fname(t), category=si['cat'], family=dict(root=tdname[si['root']], slot=s, synthetic_root=si['synthetic']),
                           defined_in=(tdname[si['definer']] if si['definer'] else (tdname[td] if si['cat'] in ('new', 'overridden') else None)), shared_users=nu,
                           shared=(nu > 1), pure=(si['cat'] == 'pure'), kind=Fu[t]['kind'], size=Fu[t]['size']))
            n_slots += 1
            n_pure += si['cat'] == 'pure'
            if not fname(t):
                all_named = False
            fam_members[(tdname[si['root']], s)].append((tdname[td], v['rva'], t))
        vts.append(dict(rva=hx(v['rva']), col_offset=v['off'], chain=[tdname[x] for x in v['chain']], chain_unresolved=v['chain_virtual'], n_slots=len(v['slots']), slots=sl))
    subs = collections.Counter(byrva[t]['subsystem'] for v in c['vtables'] for t in v['slots'] if t in byrva and byrva[t]['subsystem'])
    ctor = [hx(r) for r in B['ctor_rvas'].get(td, [])]
    dtor = [hx(r) for r in B['dtor_rvas'].get(td, [])]
    ps2 = PS['cls_loc'].get(c['logical'])
    cdb.append(dict(rtti_name=c['mangled'], name=c['name'], logical_name=c['logical'], vtable_iface=c['iface'], kind=c['kind'], template=c['template'], lambda_=c['lam'],
                    direct_bases=[tdname[b] for b in c['direct_bases']], virtual_bases=[tdname[b] for b in c['virtual_bases']], all_bases=allb, depth=c['depth'],
                    n_vtables=len(vts), n_slots=n_slots, n_pure_slots=n_pure, all_virtuals_named=bool(vts) and all_named,
                    vtables=vts, ctor_rvas=ctor, dtor_rvas=dtor, subsystem=(subs.most_common(1)[0][0] if subs else None),
                    ps2=(dict(scope=ps2['ps2'], how=ps2['how'], located_by='RTTI vtable', n_transfers=len(ps2['transfers']),
                              transfers=[dict(ps2=hex(p), rva=hx(q), conf=cf) for p, q, cf in ps2['transfers']]) if ps2 else None)))
json.dump(cdb, open(OUT + 'class_db_v2.json', 'w'), separators=(',', ':'))

# families
families = []
for (root, slot), mem in sorted(fam_members.items(), key=lambda kv: (kv[0][0], kv[0][1])):
    impls = sorted({t for _, _, t in mem})
    families.append(dict(root=root, slot=slot, n_vtables=len(mem), n_impls=len(impls), impls=[hx(t) for t in impls[:20]], classes=sorted({c for c, _, _ in mem})[:20]))
json.dump(families, open(OUT + 'families.json', 'w'), separators=(',', ':'))

# vtables + data labels
with open(OUT + 'vtables_v2.csv', 'w', newline='') as fh:
    w = csv.writer(fh)
    w.writerow(['vtable_rva', 'class', 'col_offset', 'n_slots'])
    for td, c in classes.items():
        for v in c['vtables']:
            w.writerow([hx(v['rva']), c['logical'] + ('' if v['off'] == 0 else '_base%+d' % v['off']), v['off'], len(v['slots'])])
with open(OUT + 'data_labels.csv', 'w', newline='') as fh:
    w = csv.writer(fh)
    w.writerow(['rva', 'name', 'source'])
    for d in B['stats']['data_labels']:
        w.writerow([d['rva'], d['name'], 'panacea %s+%s' % (d['via'], d['offset'])])

with open(OUT + 'data_in_code_v2.csv', 'w', newline='') as fh:
    w = csv.writer(fh)
    w.writerow(['start_rva', 'end_rva', 'kind', 'func_rva'])
    for f in U:
        for d in f['data_in_code']:
            w.writerow([hx(d[0]), hx(d[1]), d[2], hx(f['start_rva'])])
        for u in f['unreached_ranges']:
            if u[2] == 'unreached-data':
                w.writerow([hx(u[0]), hx(u[1]), u[2], hx(f['start_rva'])])

# ------------------------------------------------------------ reconciliation
allt = {x for s in R['vt_slots'].values() for x in s}
text_lo, text_hi = 0x1000, 0x1000 + 0x5773f3
outside = sorted(x for x in allt if not (text_lo <= x < text_hi))
nonpure_t = allt - {C['PURECALL']}
fam_res = C['fam']
fam_vs = {
    'S7_resolved_1725': fam_res['nonsynthetic'],
    'S7_with_None_buckets_1742': fam_res['nonsynthetic'] + 17,
    'synthetic_root_families_this_run': fam_res['synthetic'],
    'total_families_this_run': fam_res['resolved'],
    'why_1742': 'S7 put the 292 vtable slots whose base chain has no emitted primary vtable (abstract/interface bases and 15 virtual-base vtables) under root None, one bucket per slot index (0..16 = 17 buckets); with the topmost chain type as root they are %d distinct families, so 1,725 = families rooted in a type with its own vtable, 1,742 = 1,725 + 17 index buckets, and the true total is 1,992.' % fam_res['synthetic'],
}
reconcile = dict(
    types=len(classes), types_with_vtables=sum(1 for c in classes.values() if c['vtables']), types_base_only=sum(1 for c in classes.values() if not c['vtables']),
    vtables=len(R['vtables']), slots=sum(len(s) for s in R['vt_slots'].values()), unique_virtual_targets=len(allt), unique_targets_excluding_purecall=len(nonpure_t),
    purecall_slots=sum(1 for s in R['vt_slots'].values() for x in s if x == C['PURECALL']), logical_classes_with_vtables=len({c['logical'] for c in classes.values() if c['vtables']}),
    logical_classes_all=len({c['logical'] for c in classes.values()}), families=fam_vs,
    plan_5983=dict(note='PC-PORT-PLAN.md quotes 5,983 virtual functions. The early vt.py scan (scratch/exe-anatomy/vt.py) accepted slot targets only inside .text (RVA 0x1000-0x5783f3) and so dropped 7 targets that live in .nep (CLR native-entry stubs); 20,660 slots / 5,983 targets there versus 20,667 / 5,990 with the .nep targets.',
                   targets_outside_text=[hex(x) for x in outside], kinds=[Fu[x]['kind'] for x in outside]),
    slot_categories=dict(collections.Counter(v['cat'] for v in slot_info.values())),
    shared_virtual_functions=sum(1 for t, u in users.items() if len(u) > 1), virtual_functions_in_several_unrelated_classes=sum(1 for c in B['cands'].values() for q in c if q['source'] == 'rtti-vtable-shared'),
)
json.dump(reconcile, open(OUT + 'reconcile.json', 'w'), indent=1)

# ------------------------------------------------------------ coverage
real = [x for x in fin if x['tier'] == 'A-real-name']
role = [x for x in fin if x['tier'] == 'B-class-role']
vote = [x for x in fin if x['tier'] == 'C-class-vote']
named_any = [x for x in fin if x['name']]
clsl = [x for x in fin if x['cls']]
subl = [x for x in fin if x['subsystem']]
bysrc = collections.Counter(x['source'] for x in named_any)
conf_by_tier = collections.Counter((x['tier'], x['confidence']) for x in named_any)
kinds = collections.Counter(x['kind'] for x in fin)
cls_all_virt = sum(1 for c in cdb if c['vtables'] and c['all_virtuals_named'])
ps2_loc = PS['cls_loc']
subsys = collections.OrderedDict()
for x in fin:
    s = x['subsystem'] or '(unlabeled)'
    d = subsys.setdefault(s, dict(functions=0, bytes=0, seed=0, adjacency=0, callgraph=0, other=0, named=0, real=0, kinds=collections.Counter()))
    d['functions'] += 1
    d['bytes'] += x['size']
    b = x['subsystem_basis'] or ''
    d['seed' if b.startswith('seed') else ('adjacency' if b == 'adjacency' else ('callgraph' if b == 'callgraph' else 'other'))] += 1
    d['named'] += bool(x['name'])
    d['real'] += x['tier'] == 'A-real-name'
    d['kinds'][x['kind']] += 1
subsys = collections.OrderedDict(sorted(subsys.items(), key=lambda kv: -kv[1]['functions']))
for d in subsys.values():
    d['kinds'] = dict(d['kinds'])
ps2_pairs = D['ps2_added']
cov = dict(
    functions_total=len(fin), kinds=dict(kinds),
    named_real=len(real), named_role=len(role), named_class_vote=len(vote), named_any=len(named_any), class_labelled=len(clsl), subsystem_labelled=len(subl),
    unnamed=len(fin) - len(named_any), bytes_total=sum(x['size'] for x in fin), bytes_named=sum(x['size'] for x in named_any), bytes_real=sum(x['size'] for x in real),
    roots=sum(1 for x in fin if x['kind'] == 'root'), roots_real=sum(1 for x in real if x['kind'] == 'root'), roots_named_any=sum(1 for x in named_any if x['kind'] == 'root'),
    roots_with_subsystem=sum(1 for x in subl if x['kind'] == 'root'),
    classes_with_all_virtuals=cls_all_virt, types_with_vtables=sum(1 for c in cdb if c['vtables']),
    ps2_matched_classes=len(ps2_loc), ps2_classes_located_by_rtti=sum(1 for v in ps2_loc.values() if v['vtables'] > 0), ps2_classes_with_function_transfer=sum(1 for v in ps2_loc.values() if v['transfers']),
    ps2_functions_named=len(ps2_pairs), ps2_named_confident=sum(1 for v in ps2_pairs.values() if v['conf'] in ('high', 'medium')), ps2_named_high=sum(1 for v in ps2_pairs.values() if v['conf'] == 'high'),
    ps2_holdout=PS['hold'], ps2_anchors=PS['nanchor'], ps2_class_agreement=PS['agree_stats'],
    by_source=dict(bysrc), conf_by_tier={'%s/%s' % k: v for k, v in conf_by_tier.items()},
    vote_holdout=D['vote_holdout'], ps2_class_scoped=D['cs_stats'], subsystems=subsys, s7_revalidation=B['stats']['s7_revalidation'], s7_seed=D['s7_seed'], vote_rounds=D['vote_rounds'],
    extents=dict(s7_old_size_differs=sum(1 for x in fin if x['s7'] and x['s7']['size'] != x['size']), roots_with_fragments=sum(1 for f in U if f['fragments']), bytes_added_by_merging=sum(f['size'] - f['s7_size'] for f in U if f['has_pdata'] and f['s7_size']),
                 data_in_code_functions=sum(1 for x in fin if x['data_in_code_bytes']), data_in_code_bytes=sum(x['data_in_code_bytes'] for x in fin)),
    panacea=B['stats']['panacea'], docs_not_function_starts=B['stats']['docs_not_function_starts'], name_conflicts=D['conflicts'],
    panacea_status=dict(collections.Counter(p['status'] for p in B['stats']['panacea'])),
)
agree = dis = 0
dis_ex = []
cls_same = 0
for x in fin:
    if x['s7'] and x['s7']['name'] and x['source'] in ('rtti-vtable', 'rtti-vtable-thunk') :
        o = x['s7']['name']
        if o == x['name']:
            agree += 1
        else:
            dis += 1
            if o.split('::')[:-1] == (x['name'] or '').split('::')[:-1]:
                cls_same += 1
            if len(dis_ex) < 6:
                dis_ex.append((x['rva'], o, x['name']))
cov['s7_vfn_name_agreement'] = dict(identical=agree, different=dis, different_but_same_class=cls_same, examples=dis_ex)
json.dump(cov, open(OUT + 'coverage.json', 'w'), indent=1)
json.dump({k: v for k, v in subsys.items()}, open(OUT + 'subsystems.json', 'w'), indent=1)

# ------------------------------------------------------------ work list (unresolved)
def unresolved(x):
    return x['kind'] in ('root', 'leaf-no-pdata') and x['tier'] != 'A-real-name'


def completely_unnamed(x):
    return x['kind'] in ('root', 'leaf-no-pdata') and not x['name']


ncall = {r: Fu[r] for r in Fu}
top_size = sorted((x for x in fin if completely_unnamed(x)), key=lambda x: -x['size'])[:50]
top_called = sorted((x for x in fin if completely_unnamed(x)), key=lambda x: -x['n_callers'])[:50]
role_only_big = sorted((x for x in fin if x['kind'] in ('root', 'leaf-no-pdata') and x['tier'] in ('B-class-role', 'C-class-vote')), key=lambda x: -x['size'])[:50]
# virtual families without any real-named member
fam_rank = []
for (root, slot), mem in fam_members.items():
    impls = {t for _, _, t in mem if t != C['PURECALL']}
    if not impls:
        continue
    real_named = any(byrva[t]['tier'] == 'A-real-name' for t in impls)
    if real_named:
        continue
    tot = sum(Fu[t]['size'] for t in impls)
    sub = collections.Counter(byrva[t]['subsystem'] for t in impls if byrva[t]['subsystem'])
    fam_rank.append(dict(root=root, slot=slot, n_vtables=len(mem), n_impls=len(impls), bytes=tot, subsystem=(sub.most_common(1)[0][0] if sub else None), largest=hx(max(impls, key=lambda t: Fu[t]['size']))))
fam_rank.sort(key=lambda d: (-d['n_impls'], -d['bytes']))
by_sub = collections.defaultdict(lambda: dict(unnamed_fn=0, unnamed_bytes=0, unresolved_families=0, top=[]))
for x in fin:
    if completely_unnamed(x):
        d = by_sub[x['subsystem'] or '(unlabeled)']
        d['unnamed_fn'] += 1
        d['unnamed_bytes'] += x['size']
for f in fam_rank:
    by_sub[f['subsystem'] or '(unlabeled)']['unresolved_families'] += 1
for x in sorted(top_size + top_called, key=lambda x: -x['size']):
    d = by_sub[x['subsystem'] or '(unlabeled)']
    if not any(t['rva'] == x['rva'] for t in d['top']):
        d['top'].append(dict(rva=x['rva'], size=x['size'], callers=x['n_callers'], kind=x['kind'], universe_confidence=x['universe_confidence']))
wl = dict(top50_largest_unnamed=[dict(rva=x['rva'], size=x['size'], callers=x['n_callers'], subsystem=x['subsystem'], kind=x['kind'], universe_confidence=x['universe_confidence'], callees=x['n_callees']) for x in top_size],
          top50_most_called_unnamed=[dict(rva=x['rva'], size=x['size'], callers=x['n_callers'], subsystem=x['subsystem'], kind=x['kind'], universe_confidence=x['universe_confidence'], callees=x['n_callees']) for x in top_called],
          top50_largest_role_named_only=[dict(rva=x['rva'], size=x['size'], name=x['name'], subsystem=x['subsystem'], confidence=x['confidence']) for x in role_only_big],
          virtual_families_without_real_name=len(fam_rank), virtual_families_total=len(fam_members), top_virtual_families=fam_rank[:60], by_subsystem=by_sub)
json.dump(wl, open(OUT + 'worklist.json', 'w'), indent=1)

# markdown reports
L = []
L.append('# Name DB v2 coverage report\n')
L.append('Input: `pc/kh2fm.exe` sha256 9002b2de6a1f91a7, corrected canonical universe (38,451 functions). Everything below is static analysis.\n')
L.append('| quantity | count |\n|---|---:|')
for k in ('functions_total', 'named_real', 'named_role', 'named_class_vote', 'named_any', 'class_labelled', 'subsystem_labelled', 'unnamed', 'roots', 'roots_real', 'roots_named_any', 'roots_with_subsystem', 'classes_with_all_virtuals', 'types_with_vtables',
          'ps2_matched_classes', 'ps2_classes_located_by_rtti', 'ps2_classes_with_function_transfer', 'ps2_functions_named', 'ps2_named_confident', 'ps2_named_high'):
    L.append('| %s | %s |' % (k, '{:,}'.format(cov[k])))
L.append('\n## Names by source\n\n| source | functions |\n|---|---:|')
for k, v in sorted(bysrc.items(), key=lambda kv: -kv[1]):
    L.append('| %s | %s |' % (k, '{:,}'.format(v)))
L.append('\n## Name tier x confidence\n\n| tier/confidence | functions |\n|---|---:|')
for k, v in sorted(cov['conf_by_tier'].items()):
    L.append('| %s | %s |' % (k, '{:,}'.format(v)))
L.append('\n## Subsystems\n\n| subsystem | functions | bytes | seed | adjacency | call-graph | named | real names |\n|---|---:|---:|---:|---:|---:|---:|---:|')
for k, d in subsys.items():
    L.append('| %s | %s | %s | %s | %s | %s | %s | %s |' % (k, '{:,}'.format(d['functions']), '{:,}'.format(d['bytes']), d['seed'], d['adjacency'], d['callgraph'], d['named'], d['real']))
L.append('\n## Extent fixes\n\n```\n%s\n```' % json.dumps(cov['extents'], indent=1))
L.append('\n## S7 re-validation\n\n```\n%s\n```' % json.dumps(cov['s7_revalidation'], indent=1))
L.append('\n## Panacea (OpenKH) byte patterns\n\n| pattern | status | hits | function starts |\n|---|---|---|---|')
for p in cov['panacea']:
    L.append('| %s | %s | %s | %s |' % (p['name'], p['status'], ', '.join(p['hits'][:3]) or '-', ', '.join(p['function_starts'][:3]) or '-'))
L.append('\n## PS2 transfer\n\n```\n%s\n```' % json.dumps(dict(anchors=cov['ps2_anchors'], holdout=cov['ps2_holdout'], class_agreement=cov['ps2_class_agreement']), indent=1))
open(OUT + 'coverage.md', 'w').write('\n'.join(L) + '\n')

W = []
W.append('# Unresolved work list for clean rewriting (batch B3)\n')
W.append('"Unnamed" = no name of any tier. Funclets and thunks are excluded (they follow their parent/target). Sizes are merged extents.\n')
W.append('## Top 50 largest unnamed functions\n\n| rva | size | callers | callees | subsystem | universe conf |\n|---|---:|---:|---:|---|---|')
for x in wl['top50_largest_unnamed']:
    W.append('| %s | %d | %d | %d | %s | %s |' % (x['rva'], x['size'], x['callers'], x['callees'], x['subsystem'], x['universe_confidence']))
W.append('\n## Top 50 most-called unnamed functions\n\n| rva | size | callers | callees | subsystem | universe conf |\n|---|---:|---:|---:|---|---|')
for x in wl['top50_most_called_unnamed']:
    W.append('| %s | %d | %d | %d | %s | %s |' % (x['rva'], x['size'], x['callers'], x['callees'], x['subsystem'], x['universe_confidence']))
W.append('\n## Virtual families without any real-named implementation (%d of %d)\n\nTop 60 by number of distinct implementations.\n\n| root class | slot | vtables | impls | bytes | subsystem | largest impl |\n|---|---:|---:|---:|---:|---|---|' % (len(fam_rank), len(fam_members)))
for f in fam_rank[:60]:
    W.append('| %s | %d | %d | %d | %d | %s | %s |' % (f['root'], f['slot'], f['n_vtables'], f['n_impls'], f['bytes'], f['subsystem'], f['largest']))
W.append('\n## By subsystem\n\n| subsystem | unnamed fns | unnamed bytes | unresolved families |\n|---|---:|---:|---:|')
for k, d in sorted(by_sub.items(), key=lambda kv: -kv[1]['unnamed_bytes']):
    W.append('| %s | %d | %d | %d |' % (k, d['unnamed_fn'], d['unnamed_bytes'], d['unresolved_families']))
open(OUT + 'worklist.md', 'w').write('\n'.join(W) + '\n')
print(json.dumps({k: cov[k] for k in ('functions_total', 'named_real', 'named_role', 'named_class_vote', 'named_any', 'class_labelled', 'subsystem_labelled', 'unnamed', 'classes_with_all_virtuals', 'ps2_matched_classes', 'ps2_functions_named', 'ps2_named_confident', 'ps2_classes_with_function_transfer')}, indent=1))
print(json.dumps(reconcile['families'], indent=1)[:900])
print('families', len(fam_members), 'unresolved', len(fam_rank))
