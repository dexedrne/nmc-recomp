#!/usr/bin/env python3
"""Generate the mod static-edit table consumed by runtime/mod_static.c.

  gen_mod_static.py --edits edit_list.json --exe kh2fm.exe --universe DIR --funcs RVA,RVA,... --inline RVA,RVA --out gen/mod_static_table.h
                    [--report gen/mod_static_classify.json]

Input: the mod's decoded static edit list (89 edits; scratch/mod-gummi/edit_list.json), the pinned vanilla exe and the function
universe (functions.tsv).  Output: a C header with every FILE-BACKED edit (old bytes, new bytes, section class, owning function),
the 4 KiB `.nmc` buffer image the patched image expects right after the vanilla image, and the list of universe functions whose
bytes change (they are re-lifted by tools/mod_static_lift.py).  The header is derived from the mod: it stays local (gen/), is
never committed.  Standard library only.

Every claim in the edit list is re-checked here: exe sha256, the old bytes of each edit in the vanilla file, section membership,
that no edit lies in a function's data range, and that the set of changed functions (plus the inline detour-site functions given
with --inline) equals --funcs exactly.  Any mismatch is a hard error.
"""
import sys, os, json, struct, hashlib, argparse, bisect

def die(msg):
    sys.stderr.write('gen_mod_static: ERROR: %s\n' % msg); sys.exit(1)

class PE:
    def __init__(self, path):
        self.raw = open(path, 'rb').read(); r = self.raw
        if r[:2] != b'MZ': die('%s: not an MZ file' % path)
        pe = struct.unpack_from('<I', r, 0x3c)[0]
        if r[pe:pe + 4] != b'PE\0\0': die('%s: no PE signature' % path)
        nsec, optsz = struct.unpack_from('<H', r, pe + 6)[0], struct.unpack_from('<H', r, pe + 20)[0]
        oh = pe + 24
        if struct.unpack_from('<H', r, oh)[0] != 0x20b: die('%s: not PE32+' % path)
        self.base = struct.unpack_from('<Q', r, oh + 24)[0]; self.size_of_image = struct.unpack_from('<I', r, oh + 56)[0]
        self.secs = []
        for i in range(nsec):
            sh = oh + optsz + 40 * i
            name = r[sh:sh + 8].rstrip(b'\0').decode()
            vsz, va, rsz, raw = struct.unpack_from('<IIII', r, sh + 8); ch = struct.unpack_from('<I', r, sh + 36)[0]
            self.secs.append((name, va, vsz, raw, rsz, ch))
    def sec(self, rva):
        for name, va, vsz, raw, rsz, ch in self.secs:
            if va <= rva < va + max(vsz, rsz): return name
    def file_off(self, rva, n):
        """file offset of n bytes at rva, or None when any byte is not file-backed (BSS tail / headers)."""
        for name, va, vsz, raw, rsz, ch in self.secs:
            if va <= rva and rva + n <= va + min(vsz, rsz): return raw + rva - va
        return None

def load_universe(d):
    """-> sorted [(lo, hi, start, [(data ranges)])] from functions.tsv (one extent per function)."""
    fs = []
    with open(os.path.join(d, 'functions.tsv')) as f:
        hdr = f.readline().rstrip('\n').split('\t'); ix = {n: i for i, n in enumerate(hdr)}
        for ln in f:
            p = ln.rstrip('\n').split('\t')
            if p[ix['nextents']] != '1': die('universe: function %s has %s extents' % (p[ix['start_rva']], p[ix['nextents']]))
            lo, hi = (int(x, 16) for x in p[ix['extents']].split('-'))
            fs.append((lo, hi, int(p[ix['start_rva']], 16)))
    fs.sort()
    return fs

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--edits', required=True); ap.add_argument('--exe', required=True); ap.add_argument('--universe', required=True)
    ap.add_argument('--funcs', required=True, help='expected re-lifted functions (hex rvas, comma or space separated)')
    ap.add_argument('--inline', required=True, help='functions that contain an inline detour site (subset of --funcs)')
    ap.add_argument('--chained', default='', help='functions already owned by a runtime hook (the hook forwards to mod_f_<rva>)')
    ap.add_argument('--out', required=True); ap.add_argument('--report')
    a = ap.parse_args()
    E = json.load(open(a.edits)); pe = PE(a.exe)
    sha = hashlib.sha256(pe.raw).hexdigest()
    if sha != E['exe_sha256']: die('exe sha256 %s is not the pinned build %s' % (sha, E['exe_sha256']))
    if pe.base != int(E['image_base'], 16): die('image base')
    want = sorted(int(x, 16) for x in a.funcs.replace(',', ' ').split())
    inline = sorted(int(x, 16) for x in a.inline.replace(',', ' ').split())
    chained = sorted(int(x, 16) for x in a.chained.replace(',', ' ').split())
    fs = load_universe(a.universe); los = [f[0] for f in fs]
    def owner(rva):
        i = bisect.bisect_right(los, rva) - 1; r = []
        while i >= 0 and rva - fs[i][0] < 0x100000:
            if fs[i][0] <= rva < fs[i][1]: r.append(fs[i][2])
            i -= 1
        return r
    nmc_rva = pe.size_of_image; nmc_size = E['nmc_buffer']['size']
    if nmc_rva & 0xfff or nmc_size != 0x1000: die('.nmc placement: rva 0x%x size 0x%x' % (nmc_rva, nmc_size))
    nmc = bytes.fromhex(E['nmc_buffer']['nonzero_hex'])
    if len(nmc) > nmc_size: die('.nmc content larger than the buffer')
    KIND = {'.text': 0, '.rdata': 1, '.data': 2}
    rows = []; runtime_only = []; changed = {}
    for n, e in enumerate(E['edits']):
        rva = int(e['rva'], 16); ln = e['length']; old = bytes.fromhex(e['old']); cls = e['cls']
        if len(old) != ln: die('edit %d: old length' % n)
        sec = pe.sec(rva)
        if sec != e['section']: die('edit %d at 0x%x: section %s, edit list says %s' % (n, rva, sec, e['section']))
        fo = pe.file_off(rva, ln)
        if cls == 'runtime_only_bss':
            # the value is a heap pointer chosen at run time (script syscall table, runtime/mod_syscalls.c); the vanilla bytes are zero
            if sec != '.data' or old != bytes(ln): die('edit %d at 0x%x: runtime-only edit shape' % (n, rva))
            runtime_only.append(dict(rva=rva, len=ln, new=e['new'], group=e['group'])); continue
        if fo is None: die('edit %d at 0x%x is not file-backed' % (n, rva))
        if fo != int(e['file_offset'], 16): die('edit %d at 0x%x: file offset' % (n, rva))
        if pe.raw[fo:fo + ln] != old: die('edit %d at 0x%x: vanilla bytes %s, edit list says %s' % (n, rva, pe.raw[fo:fo + ln].hex(), old.hex()))
        if cls == 'rel32_to_new_section':
            if ln != 4 or sec != '.text': die('edit %d: rel32 edit shape' % n)
            off = int(e['buffer_offset'], 16)
            if not 0 <= off < nmc_size: die('edit %d: buffer offset' % n)
            new = struct.pack('<i', nmc_rva + off - (rva + 4)); rel = 1
        elif cls == 'literal':
            new = bytes.fromhex(e['new']); rel = 0
        else: die('edit %d: unknown class %s' % (n, cls))
        if len(new) != ln or new == old: die('edit %d at 0x%x: new bytes' % (n, rva))
        if ln > 32: die('edit %d: longer than the table field' % n)
        own = owner(rva)
        if sec == '.text':
            if len(own) != 1 or owner(rva + ln - 1) != own: die('code edit %d at 0x%x: owning functions %s' % (n, rva, [hex(x) for x in own]))
            changed.setdefault(own[0], []).append(rva)
        elif own: die('data edit %d at 0x%x lies inside function 0x%x' % (n, rva, own[0]))
        if sec not in KIND: die('edit %d at 0x%x: unexpected section %s' % (n, rva, sec))
        rows.append(dict(rva=rva, len=ln, kind=KIND[sec], rel=rel, func=own[0] if own else 0, old=old, new=new, group=e['group']))
    # detour sites inside the inline-site functions (the two mid-function Detours attachments)
    sites = {}
    for r in E['detour_hook_regions']:
        rva = int(r['rva'], 16); own = owner(rva)
        if len(own) == 1 and own[0] in inline and own[0] != rva: sites.setdefault(own[0], []).append(rva)
    for f in inline:
        if len(sites.get(f, [])) != 1: die('inline-site function 0x%x: detour regions inside it = %s' % (f, [hex(x) for x in sites.get(f, [])]))
        if f in changed: die('inline-site function 0x%x also has static edits (not expected)' % f)
    got = sorted(set(changed) | set(inline))
    if got != want:
        die('changed-function set differs from --funcs:\n  computed: %s\n  --funcs:  %s' % (' '.join('%x' % x for x in got), ' '.join('%x' % x for x in want)))
    for c in chained:
        if c not in want: die('--chained 0x%x is not in --funcs' % c)
    rows.sort(key=lambda r: r['rva'])
    for x, y in zip(rows, rows[1:]):
        if x['rva'] + x['len'] > y['rva']: die('overlapping edits at 0x%x' % y['rva'])
    cnt = [sum(1 for r in rows if r['kind'] == k) for k in range(3)]
    byt = [sum(r['len'] for r in rows if r['kind'] == k) for k in range(3)]
    def arr(b, n=32): return '{' + ','.join('0x%02x' % x for x in b) + '}'
    o = []
    o.append('/* GENERATED by tools/gen_mod_static.py from the mod edit list: mod-derived, local only, never commit. */')
    o.append('#pragma once')
    o.append('#define KH_MOD_STATIC_EXE_SHA256 "%s"' % sha)
    o.append('#define KH_MOD_STATIC_NEDITS %d      /* file-backed edits: %d code (.text), %d .rdata, %d .data */' % (len(rows), cnt[0], cnt[1], cnt[2]))
    o.append('#define KH_MOD_STATIC_NCODE %d\n#define KH_MOD_STATIC_NRDATA %d\n#define KH_MOD_STATIC_NDATA %d' % tuple(cnt))
    o.append('#define KH_MOD_STATIC_CODE_BYTES %d\n#define KH_MOD_STATIC_RDATA_BYTES %d\n#define KH_MOD_STATIC_DATA_BYTES %d' % tuple(byt))
    o.append('#define KH_MOD_STATIC_NRUNTIME_ONLY %d /* run-time values (script table pointer, zero in the file): runtime/mod_syscalls.c */' % len(runtime_only))
    o.append('#define KH_MOD_NMC_RVA 0x%xu\n#define KH_MOD_NMC_SIZE 0x%xu\n#define KH_MOD_NMC_INIT_LEN %d' % (nmc_rva, nmc_size, len(nmc)))
    o.append('static const unsigned char kh_mod_nmc_init[KH_MOD_NMC_INIT_LEN] = %s;' % arr(nmc))
    o.append('enum { KH_MOD_EDIT_CODE=0, KH_MOD_EDIT_RDATA=1, KH_MOD_EDIT_DATA=2 };')
    o.append('struct kh_mod_static_edit { unsigned rva; unsigned char len, kind, rel32; unsigned func; const char *group; unsigned char old[32], new_[32]; };')
    o.append('static const struct kh_mod_static_edit kh_mod_static_edits[KH_MOD_STATIC_NEDITS] = {')
    for r in rows:
        o.append('  {0x%x,%d,%d,%d,0x%x,"%s",%s,%s},' % (r['rva'], r['len'], r['kind'], r['rel'], r['func'], r['group'].replace('"', ''), arr(r['old']), arr(r['new'])))
    o.append('};')
    o.append('#define KH_MOD_STATIC_NFUNCS %d' % len(want))
    o.append('static const unsigned kh_mod_static_funcs[KH_MOD_STATIC_NFUNCS] = {%s};' % ','.join('0x%x' % x for x in want))
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    open(a.out, 'w').write('\n'.join(o) + '\n')
    if a.report:
        json.dump(dict(exe_sha256=sha, nmc=dict(rva=nmc_rva, size=nmc_size, init_len=len(nmc)), counts=dict(code=cnt[0], rdata=cnt[1], data=cnt[2]),
                       bytes=dict(code=byt[0], rdata=byt[1], data=byt[2]), runtime_only=runtime_only,
                       functions={('%x' % f): ['%x' % x for x in changed.get(f, [])] for f in want},
                       inline_sites={('%x' % f): '%x' % sites[f][0] for f in inline}, chained=['%x' % c for c in chained],
                       edits=[dict(rva='%x' % r['rva'], len=r['len'], section=['.text', '.rdata', '.data'][r['kind']], rel32=r['rel'], func='%x' % r['func'],
                                   old=r['old'].hex(), new=r['new'].hex(), group=r['group']) for r in rows]), open(a.report, 'w'), indent=1)
    print('gen_mod_static: %d file-backed edits (%d code / %d .rdata / %d .data; %d+%d+%d bytes), %d runtime-only, .nmc 0x%x+0x%x (%d init bytes), %d functions to re-lift (%d with static edits + %d inline sites)'
          % (len(rows), cnt[0], cnt[1], cnt[2], byt[0], byt[1], byt[2], len(runtime_only), nmc_rva, nmc_size, len(nmc), len(want), len(changed), len(inline)))

if __name__ == '__main__':
    main()
