#!/usr/bin/env python3
"""hd_modlist.py - provenance list for the HD asset layer of a BAKED tree (vanilla extraction + mod laid over it).

  hd_modlist.py --tree DIR (--manifest FILE | --newer 'YYYY-MM-DD HH:MM') --out FILE [--expect-mod N] [--report FILE]

The runtime (runtime/hd_assets.c, KHNMC_HD_MODLIST=<out file>) needs to know which files of the merged tree are the mod's, because
OpenKH Panacea pairs a game file with the HD files of the MOD's remastered folder, not with the union the bake produced.

  mod <path>    every file that came from the mod.  Source of truth: the bake manifest (tools/bake_nmc_tree.py writes
                <out>.manifest.json with "mod_files").  When the manifest is not at hand, --newer takes every file modified at or
                after the given local time: a STOPGAP that only holds for a tree whose vanilla files are all older than the bake
                (check the printed counts against the mod's file count; --expect-mod makes a mismatch an error).
  nohd <path>   a mod game file that has NO mod HD files, whose vanilla HD set must not be attached because the mod moved the
                textures inside the file.  Evidence used here: the same file exists for other languages (.../us/... -> fr, gr, it,
                sp, uk, fm, jp), those vanilla copies all have the same image-entry layout (type, offset, size of the BAR entries of
                type 7 TIM, 10 TM2, 18 PAX, 24 IMD, 29 IMZ), and the mod's copy has another.  The package's HD entries carry the
                vanilla offsets; a texture that moved is not found by them, the game uses the image inside the mod's file.
                Files without language siblings (obj/*.mdlx ...) get no line: nothing in the tree says where vanilla had them.

Nothing is written into the tree.  Exit status 0, or 2 on usage / verification errors."""
import argparse, json, os, struct, sys, time

LANGS = ('us', 'fr', 'gr', 'it', 'sp', 'uk', 'fm', 'jp')
IMAGE_TYPES = (7, 10, 18, 24, 29)


def image_layout(path):
    """(type, offset, size) of the image entries of a BAR container, or None when the file is no BAR."""
    with open(path, 'rb') as f:
        d = f.read()
    if d[:4] != b'BAR\x01' or len(d) < 16:
        return None
    n = struct.unpack_from('<I', d, 4)[0]
    if 16 + 16 * n > len(d):
        return None
    out = []
    for i in range(n):
        t = struct.unpack_from('<H', d, 16 + 16 * i)[0]
        off, size = struct.unpack_from('<2I', d, 24 + 16 * i)
        if t in IMAGE_TYPES:
            out.append((t, off, size))
    return tuple(out)


def siblings(rel):
    parts = rel.split('/')
    idx = [i for i, p in enumerate(parts[:-1]) if p.lower() == 'us']
    if len(idx) != 1:
        return []
    return ['/'.join(parts[:idx[0]] + [lang] + parts[idx[0] + 1:]) for lang in LANGS if lang != 'us']


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--tree', required=True)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument('--manifest')
    src.add_argument('--newer')
    ap.add_argument('--out', required=True)
    ap.add_argument('--expect-mod', type=int)
    ap.add_argument('--report')
    a = ap.parse_args()
    tree = a.tree.rstrip('/')
    if not os.path.isdir(tree):
        sys.exit(f'hd_modlist: {tree} is not a directory')

    files = {}                                   # lower-case rel -> rel
    for root, dirs, names in os.walk(tree, followlinks=True):
        for nm in names:
            rel = os.path.relpath(os.path.join(root, nm), tree).replace(os.sep, '/')
            files[rel.lower()] = rel
    if a.manifest:
        with open(a.manifest) as f:
            man = json.load(f)
        mod = set()
        missing = []
        for k in man.get('mod_files', {}):
            if k.lower() in files:
                mod.add(files[k.lower()])
            else:
                missing.append(k)
        if missing:
            print(f'hd_modlist: {len(missing)} manifest files are not in the tree (first: {missing[0]})', file=sys.stderr)
            sys.exit(2)
        source = f'bake manifest {a.manifest}'
    else:
        try:
            thr = time.mktime(time.strptime(a.newer, '%Y-%m-%d %H:%M'))
        except ValueError:
            sys.exit(f"hd_modlist: --newer wants 'YYYY-MM-DD HH:MM', got {a.newer!r}")
        mod = {rel for rel in files.values() if os.stat(os.path.join(tree, rel)).st_mtime >= thr}
        source = f'STOPGAP: files modified at or after {a.newer} local time (no bake manifest given)'
    mod_hd = {m for m in mod if m.lower().startswith('remastered/')}
    mod_game = mod - mod_hd
    print(f'hd_modlist: tree {tree}: {len(files)} files; mod files: {len(mod)} = {len(mod_game)} game files + {len(mod_hd)} remastered files ({source})')
    if a.expect_mod is not None and len(mod) != a.expect_mod:
        print(f'hd_modlist: expected {a.expect_mod} mod files, found {len(mod)}: refusing to write a list', file=sys.stderr)
        sys.exit(2)

    # remastered folders by game file
    hd_dirs = {}                                 # lower-case game rel -> [remastered file rels]
    for low, rel in files.items():
        if not low.startswith('remastered/'):
            continue
        parts = rel.split('/')
        for cut in range(len(parts) - 1, 1, -1):  # the game file is the longest prefix that is a file of the tree
            g = '/'.join(parts[1:cut])
            if g.lower() in files:
                hd_dirs.setdefault(g.lower(), []).append(rel)
                break
    nohd, rep = [], []
    counts = dict(mixed=0, mod_only=0, vanilla_only_modfile=0, moved=0, same=0, no_evidence=0)
    for glow, hd in sorted(hd_dirs.items()):
        g = files[glow]
        n_mod = sum(1 for h in hd if h in mod_hd)
        if g not in mod_game:
            continue
        if n_mod:
            counts['mixed' if n_mod < len(hd) else 'mod_only'] += 1
            if n_mod < len(hd):
                rep.append(f'mixed\t{g}\tmod HD files {n_mod}, vanilla HD files {len(hd) - n_mod}')
            continue
        counts['vanilla_only_modfile'] += 1
        mine = image_layout(os.path.join(tree, g))
        sib = [(s, image_layout(os.path.join(tree, files[s.lower()]))) for s in siblings(g) if s.lower() in files and files[s.lower()] not in mod]
        sib = [(s, lay) for s, lay in sib if lay is not None]
        lays = {lay for _, lay in sib}
        if mine is not None and len(sib) >= 2 and len(lays) == 1:
            ref = next(iter(lays))
            if mine != ref:
                counts['moved'] += 1
                why = 'textures moved: this file ' + ' '.join(f'type{t}@{o:#x}+{s:#x}' for t, o, s in mine) + f'; the {len(sib)} vanilla language copies ' + ' '.join(f'type{t}@{o:#x}+{s:#x}' for t, o, s in ref)
                nohd.append((g, why))
                rep.append(f'nohd\t{g}\t{why}')
            else:
                counts['same'] += 1
                rep.append(f'same-layout\t{g}\tas the {len(sib)} vanilla language copies: vanilla HD set kept ({len(hd)} files)')
        else:
            counts['no_evidence'] += 1
            rep.append(f'no-evidence\t{g}\t{len(hd)} vanilla HD files; ' + ('no BAR container' if mine is None else f'{len(sib)} vanilla language copies with {len(lays)} different layouts'))
    with open(a.out, 'w') as f:
        f.write(f'# provenance list for KHNMC_HD_MODLIST (tools/hd_modlist.py)\n# tree: {tree}\n# source: {source}\n')
        f.write(f'# {len(mod)} mod files, {len(nohd)} nohd rules\n')
        for g, why in nohd:
            f.write(f'nohd {g}  # {why}\n')
        for m in sorted(mod):
            f.write(f'mod {m}\n')
    print(f'hd_modlist: mod game files with a remastered folder: only mod HD files {counts["mod_only"]}, mod + vanilla HD files {counts["mixed"]}, only vanilla HD files {counts["vanilla_only_modfile"]}')
    print(f'hd_modlist: of the last: textures moved (nohd) {counts["moved"]}, same layout as the vanilla language copies {counts["same"]}, no evidence in the tree {counts["no_evidence"]}')
    print(f'hd_modlist: wrote {a.out}: {len(mod)} mod lines, {len(nohd)} nohd lines')
    if a.report:
        with open(a.report, 'w') as f:
            f.write('\n'.join(rep) + '\n')
        print(f'hd_modlist: per-file report in {a.report}')


if __name__ == '__main__':
    main()
