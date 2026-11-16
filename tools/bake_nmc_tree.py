#!/usr/bin/env python3
"""bake_nmc_tree.py - lay an OpenKH mod (mod.yml, `copy` assets) over a vanilla extracted KH2 data tree.

  bake_nmc_tree.py --vanilla DIR --mod DIR --out DIR [--link] [--drop NAME ...] [--no-hash] [--dry-run]

Semantics follow OpenKH (Patcher + Panacea, see docs/OPENKH-MOD-APPLICATION.md): every `copy` asset in mod.yml replaces or
adds the file `<name>` in the output tree; a `source` with `type: internal` is taken from the vanilla tree instead of the mod.
Other methods (bar, binarc, kh2msg, listpatch, ...) are NOT implemented here: the tool refuses them rather than guess
(run OpenKH's Patcher for those).  Windows extractions are case-insensitive: asset names are matched to existing vanilla paths
case-insensitively so that no duplicate differently-cased files appear on Linux.

--link   hardlink vanilla files into --out instead of copying (needs same filesystem; mod files are written as new files)
--drop   remove top-level directories (e.g. gumibattle gumiblock gumimenu) from the output tree
Nothing is ever written into the vanilla tree or the mod repo.  Output + manifest stay local (never commit them).
"""
import argparse, hashlib, json, os, re, shutil, sys, time


def read_assets(modyml):
    text = open(modyml, newline=None, errors='replace').read().replace('\r\n', '\n').replace('\r', '\n')
    try:
        import yaml
        data = yaml.safe_load(text)
        return data.get('assets') or []
    except ImportError:
        pass
    assets, cur = [], None
    for line in text.split('\n'):
        m = re.match(r'^- name:\s*(.+?)\s*$', line)
        if m:
            cur = {'name': m.group(1).strip('"\''), 'source': []}
            assets.append(cur)
            continue
        if cur is None:
            continue
        m = re.match(r'^\s+method:\s*(\S+)', line)
        if m:
            cur['method'] = m.group(1)
        m = re.match(r'^\s+-\s+name:\s*(.+?)\s*$', line)
        if m:
            cur['source'].append({'name': m.group(1).strip('"\'')})
        m = re.match(r'^\s+type:\s*(\S+)', line)
        if m and cur['source']:
            cur['source'][-1]['type'] = m.group(1)
    return assets


def sha1(path):
    h = hashlib.sha1()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 22), b''):
            h.update(b)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--vanilla', required=True)
    ap.add_argument('--mod', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--link', action='store_true')
    ap.add_argument('--drop', nargs='*', default=[])
    ap.add_argument('--no-hash', action='store_true')
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()

    van, mod, out = (os.path.abspath(p) for p in (a.vanilla, a.mod, a.out))
    assets = read_assets(os.path.join(mod, 'mod.yml'))
    bad = sorted({x.get('method', 'copy') for x in assets if x.get('method', 'copy') != 'copy'})
    if bad:
        sys.exit(f'refusing: mod.yml uses non-copy methods {bad}; run OpenKH Patcher for those')
    print(f'{len(assets)} copy assets in mod.yml')

    # case-insensitive index of the vanilla tree (relative paths, forward slashes)
    t0 = time.time()
    index = {}
    vfiles = []
    for dp, dn, fn in os.walk(van):
        for f in fn:
            rel = os.path.relpath(os.path.join(dp, f), van).replace(os.sep, '/')
            index[rel.lower()] = rel
            vfiles.append(rel)
    print(f'vanilla: {len(vfiles)} files indexed in {time.time()-t0:.0f}s')

    plan = {}          # out-relative path -> ('mod'|'vanilla', source path)
    stats = {'replaced': 0, 'added': 0, 'case_mapped': 0, 'missing_source': 0}
    missing = []
    for x in assets:
        name = x['name'].replace('\\', '/').lstrip('/')
        src = x['source'][0]
        sname = src['name'].replace('\\', '/').lstrip('/')
        if src.get('type') == 'internal':
            rel = index.get(sname.lower())
            spath = os.path.join(van, rel) if rel else None
            origin = 'vanilla'
        else:
            spath = os.path.join(mod, sname)
            if not os.path.isfile(spath):                      # case-insensitive fallback inside the mod
                cand = None
                d, b = os.path.split(spath)
                if os.path.isdir(d):
                    for e in os.listdir(d):
                        if e.lower() == b.lower():
                            cand = os.path.join(d, e)
                spath = cand
            origin = 'mod'
        if not spath or not os.path.isfile(spath):
            stats['missing_source'] += 1
            missing.append(name)
            continue
        hit = index.get(name.lower())
        if hit:
            if hit != name:
                stats['case_mapped'] += 1
            name_out = hit
            stats['replaced'] += 1
        else:
            name_out = name
            stats['added'] += 1
        plan[name_out] = (origin, spath)
    print('plan:', stats, '| dropped dirs:', a.drop)
    if missing:
        print('missing sources (first 10):', missing[:10])
    if a.dry_run:
        return

    if os.path.exists(out) and os.listdir(out):
        sys.exit(f'refusing: {out} exists and is not empty')
    os.makedirs(out, exist_ok=True)
    drop = {d.lower() for d in a.drop}
    n = 0
    for rel in vfiles:
        if rel.split('/')[0].lower() in drop:
            continue
        if rel in plan:
            continue                                            # will be written from the mod
        dst = os.path.join(out, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if a.link:
            os.link(os.path.join(van, rel), dst)
        else:
            shutil.copyfile(os.path.join(van, rel), dst)
        n += 1
        if n % 20000 == 0:
            print(f'  vanilla files placed: {n}')
    manifest = {}
    for rel, (origin, spath) in plan.items():
        if rel.split('/')[0].lower() in drop:
            continue
        dst = os.path.join(out, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(spath, dst)
        manifest[rel] = origin
    print(f'placed {n} vanilla files + {len(manifest)} mod files')

    if not a.no_hash:                                           # verify every mod-origin file byte-for-byte
        bad = 0
        for rel, (origin, spath) in plan.items():
            if rel in manifest and sha1(os.path.join(out, rel)) != sha1(spath):
                bad += 1
        print('verification: mod files differing from source =', bad)
        if bad:
            sys.exit(2)
    with open(out.rstrip('/') + '.manifest.json', 'w') as f:
        json.dump({'stats': stats, 'vanilla_files': len(vfiles), 'dropped': a.drop,
                   'mod_files': {k: v for k, v in manifest.items()}}, f)
    print('done; manifest:', out.rstrip('/') + '.manifest.json')


if __name__ == '__main__':
    main()
