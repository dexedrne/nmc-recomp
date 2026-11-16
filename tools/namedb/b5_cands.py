#!/usr/bin/env python3
"""B3 step 3: candidate name generation. Produces cands.pkl = {rva: [cand,...]} plus side tables
(panacea results, docs validation, S7 re-validation stats, string/assert map, data labels).
A cand is {name, cls, tier ('A-real-name'|'B-class-role'), conf, source, evidence, sub(optional), kind}."""
import sys, pickle, collections, re, json, struct, bisect
sys.path.insert(0, '/home/user/projects/kh2-nmc-analysis/scratch/namedb2')
from lib import *
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
import docs_names

im = Img()
img = im.img
U = load_universe()
F = {f['start_rva']: f for f in U}
R = pickle.load(open(HERE + 'rtti.pkl', 'rb'))
C = pickle.load(open(HERE + 'classes.pkl', 'rb'))
SC = pickle.load(open(HERE + 'scan.pkl', 'rb'))
STR = pickle.load(open(HERE + 'strings.pkl', 'rb'))
classes, tdname, slot_info, users, thunk_of, stores, PURECALL = C['classes'], C['tdname'], C['slot_info'], C['users'], C['thunk_of'], C['stores'], C['PURECALL']
direct, anc = C['direct'], C['anc']
cands = collections.defaultdict(list)
stats = collections.OrderedDict()
md = Cs(CS_ARCH_X86, CS_MODE_64)


def add(rva, name, cls, tier, conf, source, evidence, sub=None, kind=None):
    if rva not in F:
        stats.setdefault('dropped_not_a_function_start', []).append((hex(rva), name, source))
        return False
    cands[rva].append(dict(name=name, cls=cls, tier=tier, conf=conf, source=source, evidence=evidence, sub=sub, kind=kind))
    return True


def disp(td):
    return classes[td]['logical']


# ---------------------------------------------------------------- A. RTTI virtual functions
def shape(fn):
    f = F[fn]
    if f['kind'] == 'import-stub':
        return 'import'
    ins = list(md.disasm(img[fn:fn + min(f['size'], 24)], fn))
    txt = [(i.mnemonic, i.op_str) for i in ins]
    if txt and txt[0][0] == 'ret':
        return 'ret_void' if txt[0][1] == '' else 'ret_n'
    if len(txt) >= 2 and txt[-1][0] == 'ret':
        a = txt[0]
        if a == ('xor', 'eax, eax') and len(txt) == 2:
            return 'ret0'
        if a[0] == 'mov' and a[1].startswith(('eax, ', 'al, ', 'rax, ')) and len(txt) == 2:
            v = a[1].split(', ')[1]
            return 'ret_%s' % v.replace('0x', '')
        if a == ('mov', 'rax, rcx') and len(txt) == 2:
            return 'ret_this'
    return 'stub'


def is_deleting_dtor(fn):
    f = F[fn]
    if f['kind'] not in ('root', 'leaf-no-pdata'):
        return False
    code = img[fn:fn + min(f['size'], 80)]
    return b'\xf6\xc2\x01' in code or b'\x40\xf6\xc7\x01' in code or b'\x83\xe2\x01' in code or b'\x80\xe2\x01' in code or b'\xf6\xc3\x01' in code


vname = {}      # fn -> info
nv = 0
for t, us in users.items():
    if t == PURECALL:
        continue
    occ = collections.defaultdict(lambda: dict(slots=set(), roots=set(), n=0, offs=set(), cats=set()))
    for (td, vt, off, s) in us:
        si = slot_info[(vt, s)]
        dc = si['definer'] if si['cat'] == 'inherited' else td
        o = occ[dc]
        o['slots'].add(s)
        o['roots'].add(si['root'])
        o['n'] += 1
        o['offs'].add(off)
        o['cats'].add(si['cat'])
    vname[t] = dict(occ=occ, nusers=len(us))
shared_cnt = 0
for t, v in vname.items():
    occ = v['occ']
    f = F[t]
    if len(occ) == 1:
        dc, o = next(iter(occ.items()))
        s = min(o['slots'])
        nm = '%s::vfn%d' % (disp(dc), s)
        cls = disp(dc)
        ev = 'class %s vtable slot %d; %d users; family root %s' % (cls, s, v['nusers'], ', '.join(sorted(disp(r) for r in o['roots']))[:80])
        if 0 not in o['offs'] and len(o['offs']) >= 1:
            # secondary vtable: slot index is relative to the base subobject
            pass
        conf = 'high'
        if 0 in o['slots'] and is_deleting_dtor(t) and f['kind'] != 'thunk':
            nm = '%s::scalar_deleting_dtor' % cls
            ev += '; slot 0 with test dl,1 (deleting destructor shape)'
            cands[t].append(dict(name=nm, cls=cls, tier='B-class-role', conf='high', source='rtti-vtable', evidence=ev, sub=None, kind='deleting-dtor'))
            vname[t]['final'] = nm
            continue
        cands[t].append(dict(name=nm, cls=cls, tier='B-class-role', conf=conf, source='rtti-vtable', evidence=ev, sub=None, kind='virtual'))
        vname[t]['final'] = nm
    else:
        shared_cnt += 1
        # shared across several unrelated classes: stub/thunk shared implementation
        sh = shape(t)
        classes_n = len(occ)
        fams = len({r for o in occ.values() for r in o['roots']})
        best = max(occ.items(), key=lambda kv: (kv[1]['n'], -kv[0]))
        nm = 'VFN_SHARED::%s_%x' % (sh, t)
        ev = 'shared virtual implementation: %d classes, %d families, %d vtable slots; most frequent user %s slot %d' % (classes_n, fams, v['nusers'], disp(best[0]), min(best[1]['slots']))
        cands[t].append(dict(name=nm, cls=None, tier='B-class-role', conf='high', source='rtti-vtable-shared', evidence=ev, sub=None, kind='virtual-shared'))
        vname[t]['final'] = nm
# thunks
nthunk_impl = 0
for t, th in thunk_of.items():
    if t not in vname:
        continue
    base = vname[t].get('final')
    if base is None:
        continue
    tg = th['target']
    cands[t].append(dict(name=base + '_thunk', cls=None, tier='B-class-role', conf='high', source='rtti-vtable-thunk',
                         evidence='adjustor thunk adjust %+d -> %s' % (th['adjust'], hex(tg) if tg else '?'), sub=None, kind='virtual-thunk'))
    # drop the plain duplicate candidate for the thunk, keep the _thunk one
    cands[t] = [c for c in cands[t] if not (c['name'] == base and c['kind'] in ('virtual', 'virtual-shared'))]
    if tg and tg in F and tg not in vname and F[tg]['kind'] in ('root', 'leaf-no-pdata') and 'VFN_SHARED' not in base:
        cands[tg].append(dict(name=base + '_impl', cls=vname[t]['final'].split('::')[0] if False else None, tier='B-class-role', conf='medium',
                              source='rtti-vtable-thunk-target', evidence='target of adjustor thunk %x (%s)' % (t, base), sub=None, kind='virtual-impl'))
        nthunk_impl += 1
stats['virtual_targets_named'] = sum(1 for v in vname.values() if v.get('final'))
stats['virtual_shared'] = shared_cnt
stats['thunk_impl_named'] = nthunk_impl

# ---------------------------------------------------------------- B. ctor / dtor from vptr stores
callees = {}
callers = collections.defaultdict(set)
for r, f in F.items():
    cs = set(f['calls']) | set(f['tail_targets'])
    cs.discard(r)
    callees[r] = cs
    for c in cs:
        callers[c].add(r)
th_target = {t: th['target'] for t, th in thunk_of.items() if th['target']}


def returns_this(fn):
    f = F[fn]
    ins = list(md.disasm(img[fn:fn + f['size']], fn))
    tot = ok = 0
    for i, x in enumerate(ins):
        if x.mnemonic == 'ret':
            tot += 1
            for w in ins[max(0, i - 8):i]:
                if w.mnemonic == 'mov' and w.op_str.startswith('rax, r') and w.op_str.split(', ')[1] in ('rbx', 'rcx', 'rdi', 'rsi', 'rbp', 'r8', 'r9', 'r10', 'r11', 'r12', 'r13', 'r14', 'r15'):
                    ok += 1
                    break
    return tot > 0 and ok == tot


dels = [t for t, v in vname.items() if v.get('final', '').endswith('::scalar_deleting_dtor')]
dtor_set = set()
front = list(dels)
while front:
    d = front.pop()
    for t in callees.get(d, ()):
        t2 = th_target.get(t, t)
        if t2 in stores and t2 not in dtor_set and t2 not in dels:
            dtor_set.add(t2)
            front.append(t2)
nct = ndt = nlo = 0
ctor_rvas = collections.defaultdict(list)
dtor_rvas = collections.defaultdict(list)
depth = C['depth']


def first_store_early(fn, vt_addr_hint=None):
    """vptr store within the first 6 instructions of the function"""
    ins = list(md.disasm(img[fn:fn + min(F[fn]['size'], 40)], fn))[:6]
    return any(i.mnemonic == 'lea' and 'rip' in i.op_str for i in ins)


for fn, st in stores.items():
    if fn in vname and vname[fn].get('final', '').endswith('::scalar_deleting_dtor'):
        continue
    if F[fn]['kind'] not in ('root', 'leaf-no-pdata'):
        continue
    st0 = [x for x in st if x[2] == 0] or st
    seq = [depth[x[1]] for x in st0]
    order = None
    if len({x[1] for x in st0}) > 1:
        if all(a <= b for a, b in zip(seq, seq[1:])):
            order = 'ctor-order'          # base vptr stored first, most derived last
            td = st0[-1][1]
        elif all(a >= b for a, b in zip(seq, seq[1:])):
            order = 'dtor-order'          # most derived first, base last
            td = st0[0][1]
        else:
            td = max(st0, key=lambda x: depth[x[1]])[1]
    else:
        td = st0[0][1]
    cls = disp(td)
    ev = 'stores vptr of %s (%d stores%s)' % (cls, len(st), '' if len({x[1] for x in st}) == 1 else ', %d classes, %s' % (len({x[1] for x in st}), order or 'mixed order'))
    if fn in dtor_set:
        kind, conf, why = 'dtor', 'high', '; callee of a scalar deleting destructor'
    elif order == 'dtor-order':
        kind, conf, why = 'dtor', 'medium', '; vptr stores go derived -> base (destructor order)'
    elif returns_this(fn):
        kind, conf, why = 'ctor', 'high', '; every return path returns a register in rax (MSVC ctor returns this)'
    elif order == 'ctor-order':
        kind, conf, why = 'ctor', 'medium', '; vptr stores go base -> derived (constructor order)'
    elif first_store_early(fn):
        kind, conf, why = 'dtor', 'low', '; vptr store at entry and no this-return (destructor-like)'
    else:
        kind, conf, why = 'ctor_or_dtor', 'low', ''
    nm = '%s::%s' % (cls, 'ctor_or_dtor' if kind == 'ctor_or_dtor' else kind)
    cands[fn].append(dict(name=nm, cls=cls, tier='B-class-role', conf=conf, source='rtti+vptr-store', evidence=ev + why, sub=None, kind=kind))
    if kind == 'ctor':
        ctor_rvas[td].append(fn)
        nct += 1
    elif kind == 'dtor':
        dtor_rvas[td].append(fn)
        ndt += 1
    else:
        nlo += 1
stats['ctors'] = nct
stats['dtors'] = ndt
stats['ctor_or_dtor_low'] = nlo
stats['deleting_dtors'] = len(dels)

# ---------------------------------------------------------------- C. imports, exports, CLR
nimp = 0
for r, f in F.items():
    if f['kind'] == 'import-stub' and f['imports']:
        dll, fn = f['imports'][0].split('!', 1)
        dll = re.sub(r'\.dll$', '', dll, flags=re.I)
        add(r, 'imp::%s!%s' % (dll, fn), None, 'A-real-name', 'high', 'pe-imports', 'import address table slot %s' % f['imports'][0])
        nimp += 1
stats['import_stubs_named'] = nimp
# exports
exp = {}
pe = im.pe
pe.parse_data_directories(directories=[0])
for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
    if e.name:
        exp[e.address] = e.name.decode()
nexp = 0
mangled = [nm for nm in exp.values() if nm.startswith('?')]
demx = {}
if mangled:
    import subprocess
    r_ = subprocess.run(['llvm-undname'] + mangled, capture_output=True, text=True).stdout.split('\n\n')
    for m_, d_ in zip(mangled, r_):
        d_ = d_.strip().split('\n')[-1]
        d_ = re.sub(r'^(public|private|protected): ', '', d_)
        d_ = re.sub(r'^(static |virtual )?[\w:<>\* ]+? __cdecl ', '', d_)
        demx[m_] = re.sub(r'\(.*$', '', d_)
for rva, nm in exp.items():
    nm = demx.get(nm, nm)
    if add(rva, nm, None, 'A-real-name', 'high', 'pe-export-table', 'export directory entry (ordinal %d)' % 0):
        nexp += 1
stats['exports_total'] = len(exp)
stats['exports_named'] = nexp
# CLR native methods (S7 clr_methods.json, re-validated against the universe 'clr-native' tag)
clr = json.load(open(S7 + 'clr_methods.json'))
nclr = nclr_bad = 0
for m in clr:
    if m['kind'] != 'native' and m['kind'] != 'il':
        pass
    if m['kind'] == 'il':
        continue       # IL bodies are data, not x64 code
    rva = m['rva']
    if rva in F and 'clr-native' in F[rva]['sources']:
        nm = m['name'] if m['type'] in ('<Module>', None) else '%s::%s' % (m['type'], m['name'])
        add(rva, nm, None, 'A-real-name', 'high', 'clr-metadata-MethodDef', 'CLR MethodDef row %s with native implementation' % m['row'])
        nclr += 1
    else:
        nclr_bad += 1
stats['clr_native_named'] = nclr
stats['clr_native_rejected'] = nclr_bad

# ---------------------------------------------------------------- D. Panacea (OpenKH, Apache-2.0) byte patterns
SRC = SCR + 'openkh/src/OpenKh.Research.Panacea/OpenKH.cpp'
txt = open(SRC).read()
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


PAN_NAMES = {
    'pfn_Axa_CFileMan_LoadFile': ('Axa::CFileMan::LoadFile', 'filemgr'),
    'pfn_Axa_CFileMan_LoadFileWithSize': ('Axa::CFileMan::LoadFileWithSize', 'filemgr'),
    'pfn_Axa_CFileMan_LoadFileWithMalloc': ('Axa::CFileMan::LoadFileWithMalloc', 'filemgr'),
    'pfn_Axa_CFileMan_GetFileSize': ('Axa::CFileMan::GetFileSize', 'filemgr'),
    'pfn_Axa_AxaResourceMan_SetResourceItem': ('Axa::AxaResourceMan::SetResourceItem', 'filemgr'),
    'pfn_Axa_PackageMan_GetFileInfo': ('Axa::PackageMan::GetFileInfo', 'filemgr'),
    'pfn_Axa_CalcHash': ('Axa::CalcHash', 'filemgr'),
    'pfn_Axa_SetReplacePath': ('Axa::SetReplacePath', 'axa-platform'),
    'pfn_Axa_FreeAllPackages': ('Axa::FreeAllPackages', 'filemgr'),
    'pfn_Axa_CFileMan_GetRemasteredCount': ('Axa::CFileMan::GetRemasteredCount', 'filemgr'),
    'pfn_Axa_CFileMan_GetRemasteredEntry': ('Axa::CFileMan::GetRemasteredEntry', 'filemgr'),
    'pfn_Axa_PackageFile_GetRemasteredAsset': ('Axa::PackageFile::GetRemasteredAsset', 'filemgr'),
    'pfn_Axa_PackageFile_OpenFileImpl': ('Axa::PackageFile::OpenFileImpl', 'filemgr'),
    'pfn_Axa_AxaSoundStream__threadProc': ('Axa::AxaSoundStream::_threadProc', 'sd-audio'),
    'pfn_Axa_OpenFile': ('Axa::OpenFile', 'axa-platform'),
    'pfn_Axa_DebugPrint': ('Axa::DebugPrint', 'axa-platform'),
    'pfn_Axa_DecryptFile': ('Axa::DecryptFile', 'filemgr'),
    'pfn_Axa_DecompressFile': ('Axa::DecompressFile', 'filemgr'),
    'pfn_VAG_STREAM_play': ('VAG_STREAM::play', 'sd-audio'),
    'pfn_VAG_STREAM_fadeOut': ('VAG_STREAM::fadeOut', 'sd-audio'),
    'pfn_VAG_STREAM_setVolume': ('VAG_STREAM::setVolume', 'sd-audio'),
    'pfn_VAG_STREAM_exit': ('VAG_STREAM::exit', 'sd-audio'),
    'settingsfunc': ('Axa::PCSettings::init', 'settingmenu'),
    'volumefunc': ('Axa::applyVolumeLevels', 'sd-audio'),
    'framefunc': ('Panacea::framefunc', 'axa-platform'),
    'LaunchGameEpic': ('Axa::Launcher::LaunchGameEpic', 'axa-platform'),
    'LaunchGameSteam': ('Axa::Launcher::LaunchGameSteam', 'axa-platform'),
    'axaAppMain': ('Axa::axaAppMain', 'axa-platform'),
}
panacea = []
text_a, text_b = 0x1000, 0x1000 + 0x5773f3
textbytes = img[text_a:text_b]
for m in pat.finditer(txt):
    name, p, mask = m.group(1), unesc(m.group(2)), m.group(3)
    L = len(mask)
    # build a regex from the masked pattern, scan the whole .text (all alignments)
    rx = b''.join(re.escape(bytes([p[i]])) if mask[i] != '?' else b'.' for i in range(L))
    hits = [text_a + mm.start() for mm in re.finditer(b'(?s)' + rx, textbytes)] if True else []
    aligned = [h for h in hits if h % 16 == 0]      # Panacea steps 0x10 from the image base
    starts = [h for h in hits if h in F]
    panacea.append(dict(name=name, line=txt[:m.start()].count('\n') + 1, patlen=L, hits=[hex(h) for h in hits], aligned=[hex(h) for h in aligned],
                        function_starts=[hex(h) for h in starts], status=('match' if len(starts) == 1 and len(hits) == 1 else ('ambiguous' if hits else 'no-match'))))
    if name in ('framefunc', 'LaunchGameEpic', 'LaunchGameSteam', 'axaAppMain') and starts:
        panacea[-1]['status'] = 'launcher-pattern-rejected'
    if name in PAN_NAMES and len(starts) == 1 and len(hits) == 1 and name not in ('framefunc', 'LaunchGameEpic', 'LaunchGameSteam', 'axaAppMain'):
        nm, sub = PAN_NAMES[name]
        cl = '::'.join(nm.split('::')[:-1]) or None
        weak = L < 12
        add(starts[0], nm, cl, 'A-real-name', 'medium' if weak else 'high', 'panacea-openkh',
            'OpenKH Panacea byte pattern %s (%d bytes, %d wildcard) matches exactly once in .text, at a function start' % (name, L, mask.count('?')), sub=sub)
stats['panacea'] = panacea
# Panacea data labels (GetVarPtr / GetArrPtr offsets)
data_labels = []


def pan(name):
    for q in panacea:
        if q['name'] == name and q['status'] == 'match':
            return int(q['function_starts'][0], 16)
    return None


def rip_target(at):
    return at + 4 + struct.unpack_from('<i', img, at)[0]


for lab, fnname, off in (('Axa::PackageFileCount', 'pfn_Axa_PackageMan_GetFileInfo', 0x1A), ('Axa::LastOpenedPackage', 'pfn_Axa_CFileMan_GetRemasteredCount', 3),
                         ('Axa::PackageFiles', 'pfn_Axa_PackageMan_GetFileInfo', 0xB1), ('Axa::BasePath', 'pfn_Axa_AxaResourceMan_SetResourceItem', 0x3E),
                         ('Axa::PCSettingsPtr', 'settingsfunc', 0x2B), ('Axa::VolumeLevels', 'volumefunc', 0x2D)):
    b = pan(fnname)
    if b is not None:
        data_labels.append(dict(name=lab, rva=hex(rip_target(b + off)), via=fnname, offset=hex(off)))
stats['data_labels'] = data_labels

# ---------------------------------------------------------------- E. our own docs
nd = 0
bad = []
for rva, (nm, sub, conf, ev) in docs_names.DOCS.items():
    cl = '::'.join(nm.split('::')[:-1]) or None
    if rva in F:
        add(rva, nm, cl, 'A-real-name', conf, 'docs-spec', ev, sub=sub)
        nd += 1
    else:
        bad.append((hex(rva), nm))
stats['docs_named'] = nd
stats['docs_not_function_starts'] = bad
# gx_boundary name hints
gx = json.load(open(SCR + 'graphics-spec/gx_boundary.json'))
ngx = 0
for e in gx['entries']:
    h = e.get('name_hint')
    rva = int(e['addr'], 16)
    if h and re.match(r'^[A-Za-z_]\w*(\(|$)', h) and rva in F:
        nm = 'GX::' + re.sub(r'\(.*$', '', h)
        cands[rva].append(dict(name=nm, cls='GX', tier='A-real-name', conf='medium', source='docs-spec-gx-boundary', evidence='GX-BOUNDARY.md entry hint: %s (%s)' % (h, e['kind']), sub='gx-graphics', kind='gx-entry'))
        ngx += 1
stats['gx_entry_hints'] = ngx
# platform-seams proposals (another worker's partial notes; consumed as data, lower rank than Panacea/docs)
import csv
nps = 0
try:
    for r in csv.DictReader(open(SCR + 'platform-seams/name_proposals.csv')):
        rva = int(r['rva'], 16)
        if rva in F:
            cl = '::'.join(r['proposed_name'].split('::')[:-1]) or None
            cands[rva].append(dict(name=r['proposed_name'], cls=cl, tier='A-real-name', conf=(r['confidence'] if r['evidence'] else ('medium' if r['confidence'] == 'high' else r['confidence'])), source='platform-seams-proposal', evidence=(r['evidence'] or 'platform-seams/name_proposals.csv entry without evidence text (confidence capped at medium)'), sub=r['subsystem'], kind='seam'))
            nps += 1
except Exception as ex:
    stats['seams_error'] = str(ex)
stats['seam_proposals_consumed'] = nps

# ---------------------------------------------------------------- F. strings: __FUNCTION__-like names, file paths, [sd] tags
ks = sorted(STR)


def sfind(t):
    i = bisect.bisect_right(ks, t) - 1
    if i >= 0 and ks[i] <= t < ks[i] + len(STR[ks[i]]) + 1:
        return ks[i]
    return None


fn_strs = collections.defaultdict(set)     # fn -> string start addresses
str_fns = collections.defaultdict(set)
for fn, s in SC.items():
    for (addr, t, mn) in s['refs']:
        k = sfind(t)
        if k is not None:
            fn_strs[fn].add(k)
            str_fns[k].add(fn)
QN = re.compile(r'^[A-Za-z_][\w]*(::[A-Za-z_~][\w]*)+$')
FILEP = re.compile(r'^[a-zA-Z]:\\.*\.(cpp|c|h|hpp|cc)$', re.I)
nstr = 0
file_of = collections.defaultdict(list)      # fn -> file strings
log_of = collections.defaultdict(list)       # fn -> [sd] tagged strings
for k, s in STR.items():
    if FILEP.match(s):
        for fn in str_fns.get(k, ()):
            file_of[fn].append(s)
    if re.match(r'^\[(sd|Sd|SD)\]', s):
        for fn in str_fns.get(k, ()):
            log_of[fn].append(s)
qn_of = collections.defaultdict(set)       # fn -> qualified-name strings it references
for k, s in STR.items():
    if QN.match(s) and len(s) > 8 and k in str_fns and len(str_fns[k]) <= 3:
        for fn in str_fns[k]:
            qn_of[fn].add(s)
ambig_fn = []
for fn, ss in qn_of.items():
    f = F[fn]
    if f['kind'] not in ('root', 'leaf-no-pdata'):
        continue
    if len(ss) > 1:
        ambig_fn.append((hex(fn), sorted(ss)[:3]))
        continue
    s = next(iter(ss))
    files = file_of.get(fn, [])
    conf = 'high' if files else 'medium'
    cl = '::'.join(s.split('::')[:-1])
    ev = '__FUNCTION__-style string %r, the only qualified-name string this function references%s' % (s, '; also references %s' % files[0].split('\\')[-1] if files else '')
    cands[fn].append(dict(name=s, cls=cl, tier='A-real-name', conf=conf, source='assert-string-fn-name', evidence=ev, sub='sd-audio' if s.startswith('SQEX::') else None, kind='fn-name-string'))
    nstr += 1
stats['ambiguous_fn_string_functions'] = len(ambig_fn)
stats['ambiguous_fn_string_examples'] = ambig_fn[:10]
stats['fn_name_strings'] = nstr
stats['functions_with_file_strings'] = len(file_of)
stats['functions_with_sd_tag_strings'] = len(log_of)
# assert-format users (the sd assert helper pattern: format string + __FILE__ + __FUNCTION__)
pickle.dump(dict(file_of=dict(file_of), log_of=dict(log_of), fn_strs={k: sorted(v) for k, v in fn_strs.items()}), open(HERE + 'strmap.pkl', 'wb'))

# ---------------------------------------------------------------- G. S7 re-validation
s7 = json.load(open(S7 + 'name_db.json'))
S7R = {int(x['rva'], 16): x for x in s7}
rev = collections.Counter()
s7_carry = 0
nt = {rec['fn']: rec for rec in json.load(open(S7 + 'nametab_records.json'))}
for rva, x in S7R.items():
    if rva not in F:
        rev['rva_not_in_corrected_universe'] += 1
        continue
    f = F[rva]
    if x['size'] != f['size']:
        rev['size_differs_merged'] += 1
    if not x['name']:
        continue
    src = x['source'] or ''
    tier = x['tier']
    if src in ('rtti-vtable', 'rtti+vptr-store', 'address-adjacency'):
        rev['superseded_by_v2_rtti(%s)' % src] += 1
        continue
    ok = True
    why = ''
    if src == 'pe-imports':
        ok = f['kind'] == 'import-stub' and bool(f['imports'])
        why = 'kind=%s' % f['kind']
    elif src == 'pe-export-table':
        ok = rva in exp
    elif src == 'clr-metadata-MethodDef':
        ok = 'clr-native' in f['sources']
    elif src.startswith('data-table'):
        ok = rva in nt or f['kind'] in ('root', 'leaf-no-pdata', 'thunk', 'trampoline')
    elif src.startswith('string-xref'):
        ok = f['kind'] in ('root',) and qn_of.get(rva) == {x['name']}
        why = 'string reference lost or ambiguous' if not ok else ''
    elif src.startswith('ps2-decomp'):
        ok = f['kind'] in ('root', 'leaf-no-pdata')
    if ok:
        rev['carried(%s)' % src.split('(')[0]] += 1
        s7_carry += 1
        cl = x['class'] or None
        cands[rva].append(dict(name=x['name'], cls=cl, tier=tier, conf=x['confidence'], source='s7:' + src, evidence=(x['evidence'] or '') + ' [S7 carried over, revalidated]', sub=x['subsystem'], kind='s7'))
    else:
        rev['rejected(%s)' % src.split('(')[0]] += 1
stats['s7_revalidation'] = dict(rev)
stats['s7_carried'] = s7_carry
pickle.dump(dict(cands=dict(cands), stats=stats, vname={k: v.get('final') for k, v in vname.items()}, S7={k: {kk: x[kk] for kk in ('name', 'class', 'subsystem', 'subsystem_basis', 'tier', 'source', 'confidence', 'size', 'ps2_addr')} for k, x in S7R.items()},
                 ctor_rvas=dict(ctor_rvas), dtor_rvas=dict(dtor_rvas), exp=exp, callees={k: sorted(v) for k, v in callees.items()}, callers={k: sorted(v) for k, v in callers.items()}),
            open(HERE + 'cands.pkl', 'wb'))
for k, v in stats.items():
    if k in ('panacea',):
        print('panacea:', collections.Counter(p['status'] for p in v))
        continue
    print(k, v if not isinstance(v, (list, dict)) or len(str(v)) < 300 else (len(v), str(v)[:300]))
