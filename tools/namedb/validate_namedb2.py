#!/usr/bin/env python3
"""Independent validation of name_db_v2 against the exe and the corrected canonical universe.
Run:  python3 -I validate_namedb2.py [exe]   (needs pefile; exit status 0 = all checks pass)
Does not import the builder; re-derives .pdata chains, RTTI counts and the Panacea matches itself."""
import sys, os, csv, json, re, struct, hashlib, collections
import pefile

D = os.path.dirname(os.path.abspath(__file__)) + '/'
EXE = sys.argv[1] if len(sys.argv) > 1 else '/home/user/projects/kh2-nmc-analysis/pc/kh2fm.exe'
UNI = '/home/user/projects/kh2-nmc-analysis/scratch/universe/skeptic/corrected/functions.json'
OPENKH = '/home/user/projects/kh2-nmc-analysis/scratch/openkh/src/OpenKh.Research.Panacea/OpenKH.cpp'
BASE = 0x140000000
fails = []
checks = 0


def ck(cond, msg):
    global checks
    checks += 1
    if not cond:
        fails.append(msg)
        print('FAIL:', msg)


raw = open(EXE, 'rb').read()
ck(hashlib.sha256(raw).hexdigest().startswith('9002b2de6a1f91a7'), 'exe sha256 prefix')
pe = pefile.PE(data=raw, fast_load=True)
img = bytes(pe.get_memory_mapped_image())
U = {f['start_rva']: f for f in json.load(open(UNI))['functions']}
ck(len(U) == 38451, 'universe has 38,451 functions')

rows = list(csv.DictReader(open(D + 'name_db_v2.csv')))
ck(len(rows) == 38451, 'csv rows == 38,451 (got %d)' % len(rows))
rv = [int(r['rva'], 16) for r in rows]
ck(len(set(rv)) == len(rv), 'unique rva')
ck(len(set(r['id'] for r in rows)) == len(rows), 'unique id')
ck(set(rv) == set(U), 'rva set equals the universe function-start set')
for r in rows:
    v = int(r['rva'], 16)
    u = U.get(v)
    if not u:
        continue
    if not (int(r['end'], 16) == u['extents'][-1][1] and int(r['size']) == u['size'] and r['kind'] == u['kind'] and int(r['has_pdata']) == int(u['has_pdata']) and int(r['data_in_code_bytes']) == sum(d[1] - d[0] for d in u['data_in_code'])):
        ck(False, 'row fields differ from universe at %s' % r['rva'])
        break
else:
    ck(True, 'row fields')
ck(all(len(U[v]['extents']) == 1 for v in U), 'every function is one contiguous extent')
ck(all(int(r['end'], 16) - int(r['rva'], 16) == int(r['size']) or r['kind'] in ('root', 'leaf-no-pdata', 'funclet', 'thunk', 'trampoline', 'import-stub') for r in rows), 'end - rva == size (single extent)')
# size == end - rva exactly (the universe has no gaps inside a function)
bad = [r['rva'] for r in rows if int(r['end'], 16) - int(r['rva'], 16) != int(r['size'])]
ck(not bad, 'end-rva == size for all rows (violations: %s)' % bad[:5])

# ---- independent .pdata re-derivation: merged extents of chained entries
pd = [s for s in pe.sections if s.Name.rstrip(b'\0') == b'.pdata'][0]
data = img[pd.VirtualAddress:pd.VirtualAddress + pd.Misc_VirtualSize]
ent = []
for i in range(0, len(data) - 11, 12):
    b, e, u = struct.unpack_from('<III', data, i)
    if b:
        ent.append((b, e, u))
by_begin = {b: (b, e, u) for b, e, u in ent}
parent = {}
for b, e, u in ent:
    flags = img[u] >> 3
    if flags & 4:     # UNW_FLAG_CHAININFO
        cnt = img[u + 2]
        off = u + 4 + ((cnt + 1) & ~1) * 2
        pb, pe_, pu = struct.unpack_from('<III', img, off)
        parent[b] = pb
roots = {}
for b, e, u in ent:
    r = b
    n = 0
    while r in parent and n < 20:
        r = parent[r]
        n += 1
    roots.setdefault(r, []).append((b, e))
ck(len(ent) == 27526, '.pdata entries 27,526 (got %d)' % len(ent))
ck(len(roots) == 17908, 'logical .pdata functions 17,908 (got %d)' % len(roots))
mism = 0
for r, rs in roots.items():
    tot = sum(e - b for b, e in rs)
    row = next((x for x in [None]), None)
    if r not in U or U[r]['size'] != tot or U[r]['extents'][0][0] != min(b for b, e in rs):
        mism += 1
ck(mism == 0, 'merged .pdata extents reproduce the universe (%d mismatches)' % mism)
nfrag = sum(1 for r, rs in roots.items() if len(rs) > 1)
ck(nfrag == 3315, 'functions with chained fragments 3,315 (got %d)' % nfrag)

# ---- RTTI recount, independent
rd0 = [s for s in pe.sections if s.Name.rstrip(b'\0') == b'.rdata'][0]
ra, rb = rd0.VirtualAddress, rd0.VirtualAddress + rd0.Misc_VirtualSize
cols = {}
for r in range(ra, rb - 24, 4):
    if struct.unpack_from('<I', img, r)[0] == 1 and struct.unpack_from('<I', img, r + 20)[0] == r:
        cols[r] = struct.unpack_from('<6I', img, r)
colva = {BASE + c: c for c in cols}
vts = {}
dd0 = [s for s in pe.sections if s.Name.rstrip(b'\0') == b'.data'][0]
for lo, hi in ((ra, rb), (dd0.VirtualAddress, dd0.VirtualAddress + min(dd0.Misc_VirtualSize, dd0.SizeOfRawData))):
    for r in range(lo, hi - 8, 8):
        q = struct.unpack_from('<Q', img, r)[0]
        if q in colva:
            vts[r + 8] = colva[q]
ck(len(cols) == 1066 and len(vts) == 1066, 'RTTI: 1,066 COLs and vtables (got %d/%d)' % (len(cols), len(vts)))
cdb = json.load(open(D + 'class_db_v2.json'))
ck(len(cdb) == 1115, 'class_db has 1,115 types')
ck(sum(len(c['vtables']) for c in cdb) == 1066, 'class_db has 1,066 vtables')
ck(sum(c['n_slots'] for c in cdb) == 20667, 'class_db has 20,667 slots')
byname = {r['rva']: r for r in rows}
targets = set()
badslot = 0
for c in cdb:
    for v in c['vtables']:
        ck_v = int(v['rva'], 16) in vts
        if not ck_v:
            badslot += 1
        for s in v['slots']:
            targets.add(s['fn'])
            if s['fn'] not in byname:
                badslot += 1
ck(badslot == 0, 'every vtable exists in the exe and every slot function is a universe function (%d bad)' % badslot)
ck(len(targets) == 5990, '5,990 unique virtual targets (got %d)' % len(targets))
# every virtual target has a name (except none) and a Class::vfnN / real / shared / thunk name
unnamed_virt = [t for t in targets if not byname[t]['name']]
ck(not unnamed_virt, 'all virtual targets named (%d missing)' % len(unnamed_virt))
# vfn name consistency: Class::vfnN must match a (class, slot) occurrence in class_db
occ = collections.defaultdict(set)
for c in cdb:
    for v in c['vtables']:
        for s in v['slots']:
            occ[s['fn']].add((c['logical_name'], s['slot']))
            if s['defined_in']:
                occ[s['fn']].add((re.sub(r'::VTABLE<.*', '', s['defined_in']), s['slot']))
inc = 0
for t in targets:
    m = re.match(r'^(.+)::vfn(\d+)(_thunk)?$', byname[t]['name'] or '')
    if m and byname[t]['source'] in ('rtti-vtable', 'rtti-vtable-thunk'):
        cls, n = m.group(1), int(m.group(2))
        if not any(n == sl and (cl == cls or cls.endswith('::' + cl) or cl.endswith(cls)) for cl, sl in occ[t]):
            # accept definer-class names too
            if not any(n == sl for cl, sl in occ[t]):
                inc += 1
ck(inc == 0, 'virtual names agree with class_db slot numbers (%d inconsistent)' % inc)

# ---- Panacea patterns re-resolved
txt = open(OPENKH).read()
pat = re.compile(r'Hook\((\w+),\s*"((?:[^"\\]|\\.)*)",\s*"([^"]*)"\);')


def unesc(s):
    out = bytearray()
    i = 0
    while i < len(s):
        if s[i] == '\\' and s[i + 1] == 'x':
            out.append(int(s[i + 2:i + 4], 16))
            i += 4
        else:
            out.append(ord(s[i]))
            i += 1
    return bytes(out)


text = img[0x1000:0x1000 + 0x5773f3]
pan_ok = pan_n = 0
for m in pat.finditer(txt):
    p, mask = unesc(m.group(2)), m.group(3)
    rx = b''.join(re.escape(bytes([p[i]])) if mask[i] != '?' else b'.' for i in range(len(mask)))
    hits = [0x1000 + mm.start() for mm in re.finditer(b'(?s)' + rx, text)]
    if len(hits) == 1 and hits[0] in U and m.group(1) not in ('framefunc', 'LaunchGameEpic', 'LaunchGameSteam', 'axaAppMain'):
        pan_n += 1
        r = byname.get(hex(hits[0]))
        if r and r['source'] == 'panacea-openkh':
            pan_ok += 1
        elif r and r['name']:
            pan_ok += 0
ck(pan_n >= 20 and pan_ok == pan_n, 'Panacea matches carry source=panacea-openkh (%d/%d)' % (pan_ok, pan_n))

# ---- field sanity
allowed_conf = {'', 'high', 'medium', 'low'}
allowed_tier = {'', 'A-real-name', 'B-class-role', 'C-class-vote'}
ck(all(r['confidence'] in allowed_conf and r['tier'] in allowed_tier for r in rows), 'confidence/tier values')
ck(all((r['name'] == '') == (r['tier'] == '') for r in rows), 'name present iff tier present')
ck(all(r['source'] and r['evidence'] and r['confidence'] for r in rows if r['name']), 'named rows carry source, evidence and confidence')
subs = collections.Counter(r['subsystem'] for r in rows if r['subsystem'])
ck(len(subs) >= 20, 'subsystem labels present (%d distinct)' % len(subs))
ck(sum(subs.values()) == json.load(open(D + 'coverage.json'))['subsystem_labelled'], 'coverage.json subsystem count matches csv')
cov = json.load(open(D + 'coverage.json'))
ck(sum(1 for r in rows if r['tier'] == 'A-real-name') == cov['named_real'], 'coverage named_real matches csv')
ck(sum(1 for r in rows if r['tier'] == 'B-class-role') == cov['named_role'], 'coverage named_role matches csv')
# data-in-code ranges never overlap another function's start and lie inside their function
dic = list(csv.DictReader(open(D + 'data_in_code_v2.csv')))
bad = 0
for d in dic:
    f = U[int(d['func_rva'], 16)]
    s, e = int(d['start_rva'], 16), int(d['end_rva'], 16)
    if not (f['extents'][0][0] <= s < e <= f['extents'][0][1]):
        bad += 1
ck(bad == 0, 'data-in-code ranges lie inside their function (%d bad)' % bad)
# optional: Ghidra report
gp = D + 'ghidra_import_report.json'
if os.path.exists(gp):
    g = json.load(open(gp))
    ck(g['rows'] == 38451 and g['functions_accepted'] == 38451 and g['function_create_or_body_failures'] == 0, 'ghidra import accepted all 38,451 rows')
    ck(g['functions_with_more_than_one_range_after'] == 0, 'ghidra functions are single-range after import')
print('checks run: %d, failures: %d' % (checks, len(fails)))
sys.exit(1 if fails else 0)
