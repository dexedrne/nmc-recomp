#!/usr/bin/env python3
"""hd_packages.py - the packages' own HD (remastered) entry tables for the runtime (runtime/hd_assets.c, KHNMC_HD_PACKAGES).

  hd_packages.py --dumps DIR --tree DIR --out FILE [--modlist FILE] [--json FILE] [--compare FILE]
  hd_packages.py --verify FILE --tree DIR [--modlist FILE]

The PC pairs a game file with HD textures through the table its package holds for that file (name, origOffset, size per entry);
OpenKH Panacea only replaces that table for a game file of the mod that has a mod remastered folder.  The loose tree has no packages,
so the tables are kept in one text file next to the data:

  path <TAB> count <TAB> name:origOffset:decompressedSize <TAB> ...      one line per game file that has entries, sorted by path

in PACKAGE order; origOffset is the full 32-bit value in hex, flag bits included (0xffffffff = no offset), names may hold ':'.

--dumps DIR   kh2_first.bin .. kh2_sixth.bin: for every HED entry of Image/dt/<name>.hed the 32-byte HED entry (16-byte MD5 of the
              game path, int64 offset, int32 dataLength, int32 actualLength), the 16-byte file header found at that offset of the
              .pkg (int32 decompressedSize, remasteredCount, compressedSize, creationDate) and remasteredCount entries of 48 bytes
              (char name[32], int32 offset, origOffset, decompressedSize, compressedSize).  Game-derived: local only, never committed.
--tree DIR    the baked tree.  Game paths are taken from it (everything outside remastered/); a path is looked up as the game does
              (GetFileInfo 0x106940 / OpenFileImpl 0x107240): the packages in registration order, in each the MD5 of the path and, on
              a miss, of the path with its file component lower-cased; the first package that has it wins.
--json FILE   pkg_entries.json made from the same dumps: must hold the same entries (cross-check).
--compare F   an existing table file: must hold the same lines (comment lines aside).
--verify F    check an existing table file against the tree instead of making one.

Verified in both modes (any failure: exit status 2, nothing kept quiet): every entry's file exists as
<tree>/remastered/<path>/<name> (exact case); names are 1..31 characters without tab; counts; and, with a provenance list (--modlist,
default <tree>/khnmc-hd-modlist.txt when present), every file that is NOT the mod's has the table's size or that size rounded up to 16
(the extraction pads; the runtime serves exactly decompressedSize bytes).  Files of the mod replace same-named package assets and may
have any size: counted.  Nothing is written into the tree."""
import argparse, hashlib, json, os, struct, sys

PACKAGES = ('kh2_first', 'kh2_second', 'kh2_third', 'kh2_fourth', 'kh2_fifth', 'kh2_sixth')   # registration order (axaAppMain 0x14f1e0)
HEADER = ('# KHNMC HD package tables: path, entry count, then name:origOffset(hex, with flag bits):decompressedSize per entry, in package order\n'
          '# source: per-file headers of Image/dt/kh2_*.pkg (tools/hd_packages.py)\n')


def die(msg):
    print('hd_packages: ' + msg, file=sys.stderr)
    sys.exit(2)


def read_dump(path):
    """{md5 hex: [(name, origOffset, decompressedSize), ...]} of one package dump."""
    with open(path, 'rb') as f:
        b = f.read()
    recs, o = {}, 0
    while o < len(b):
        if o + 48 > len(b):
            die(f'{path}: truncated record header at byte {o}')
        h = b[o:o + 16].hex()
        cnt = struct.unpack_from('<i', b, o + 36)[0]
        o += 48
        if cnt < 0 or cnt > 4096 or o + 48 * cnt > len(b):
            die(f'{path}: bad entry count {cnt} before byte {o}')
        ents = []
        for _ in range(cnt):
            raw = b[o:o + 32]
            z = raw.find(b'\0')
            name = (raw if z < 0 else raw[:z]).decode('latin-1')
            orig, size = struct.unpack_from('<Ii', b, o + 36)
            ents.append((name, orig, size))
            o += 48
        if h in recs:
            die(f'{path}: the hash {h} occurs twice')
        recs[h] = ents
    return recs


def tree_files(tree):
    """(game files, {lower-case path: path} of every file below remastered/), both with '/' separators."""
    game, hd = [], {}
    for root, dirs, names in os.walk(tree, followlinks=True):
        rel_dir = os.path.relpath(root, tree).replace(os.sep, '/')
        if rel_dir == '.':
            dirs[:] = [d for d in dirs if not d.startswith('.')]
            rel_dir = ''
        top = rel_dir.split('/')[0].lower()
        for nm in names:
            rel = (rel_dir + '/' if rel_dir else '') + nm
            if top == 'remastered':
                if rel.lower() in hd:
                    die(f'{tree}: {rel} and {hd[rel.lower()]} differ only in case (the mod\'s file system has one name for both)')
                hd[rel.lower()] = rel
            elif not (rel_dir == '' and nm in ('khnmc-hd-modlist.txt', 'khnmc-hd-packages.tsv')):
                game.append(rel)
    return sorted(game), hd


def md5(s):
    return hashlib.md5(s.encode('latin-1')).hexdigest()


def read_modlist(path):
    mod = set()
    with open(path, encoding='latin-1') as f:
        for line in f:
            line = line.strip()
            if line.startswith('mod '):
                p = line[4:].split('  #')[0].strip()
                while p.startswith('./') or p.startswith('/'):
                    p = p[2:] if p.startswith('./') else p[1:]
                mod.add(p.lower())
    if not mod:
        die(f'{path} holds no `mod` line')
    return mod


def read_tsv(path):
    tabs, order = {}, []
    with open(path, encoding='latin-1') as f:
        for ln, line in enumerate(f, 1):
            line = line.rstrip('\r\n')
            if not line or line.startswith('#'):
                continue
            f3 = line.split('\t')
            if len(f3) < 3:
                die(f'{path} line {ln}: needs path, count and at least one entry')
            try:
                cnt = int(f3[1])
            except ValueError:
                die(f'{path} line {ln}: bad count {f3[1]!r}')
            ents = []
            for e in f3[2:]:
                parts = e.rsplit(':', 2)
                if len(parts) != 3:
                    die(f'{path} line {ln}: entry {e!r} is not name:origOffset:size')
                try:
                    ents.append((parts[0], int(parts[1], 16), int(parts[2])))
                except ValueError:
                    die(f'{path} line {ln}: entry {e!r} is not name:origOffset:size')
            if cnt != len(ents):
                die(f'{path} line {ln}: {len(ents)} entries, the count says {cnt}')
            if f3[0].lower() in tabs:
                die(f'{path} line {ln}: second table for {f3[0]}')
            tabs[f3[0].lower()] = (f3[0], ents)
            order.append(f3[0])
    return tabs, order


def fmt_line(path, ents):
    return '%s\t%d\t%s' % (path, len(ents), '\t'.join('%s:%08x:%d' % e for e in ents))


def verify(tree, tables, hd, mod, game_lower):
    """tables: [(path, entries)].  Prints the totals; returns the number of faults."""
    faults, shown = 0, 0

    def fault(msg):
        nonlocal faults, shown
        faults += 1
        if shown < 40:
            print('hd_packages: FAULT ' + msg, file=sys.stderr)
            shown += 1

    n_ent = n_mod = n_mod_other = n_pad = n_exact = n_minus1 = 0
    nib = {}
    for path, ents in tables:
        if path.lower() not in game_lower:
            fault(f'{path}: no such game file in the tree')
        if not ents:
            fault(f'{path}: a table without entries')
        for name, orig, size in ents:
            n_ent += 1
            if not 1 <= len(name) <= 31 or '\t' in name or '\n' in name:
                fault(f'{path}: entry name {name!r} is not 1..31 characters without tab')
            if orig == 0xffffffff:
                n_minus1 += 1
            else:
                nib[orig >> 28] = nib.get(orig >> 28, 0) + 1
            rel = hd.get(f'remastered/{path}/{name}'.lower())          # the folder as the runtime finds it (any case), the name exactly
            if rel is None or not rel.endswith('/' + name):
                fault(f'{path}: entry {name!r} has no file remastered/{path}/{name} in the tree')
                continue
            if size < 0:
                fault(f'{path}: entry {name!r} has the size {size}')
            if mod is None:
                continue
            real = os.stat(os.path.join(tree, rel)).st_size
            if rel.lower() in mod:
                n_mod += 1
                n_mod_other += real != size
            elif real == size:
                n_exact += 1
            elif real == (size + 15) & ~15:
                n_pad += 1
            else:
                fault(f'{path}: {rel} is {real} bytes, the table says {size}: not the package asset and not a file of the mod')
    print(f'hd_packages: {len(tables)} game files, {n_ent} entries; top nibble of origOffset: '
          + ' '.join(f'0x{k:x}={v}' for k, v in sorted(nib.items())) + f'; origOffset -1: {n_minus1}')
    if mod is None:
        print('hd_packages: NO provenance list: file sizes were NOT checked (name --modlist, or put khnmc-hd-modlist.txt into the tree)')
    else:
        print(f'hd_packages: sizes: {n_exact} entries are files of exactly the table size, {n_pad} files are padded to 16 bytes, '
              f'{n_mod} are files of the mod ({n_mod_other} of them of another size than the package asset they replace)')
    if faults:
        print(f'hd_packages: {faults} FAULTS' + (f' (first {shown} shown)' if faults > shown else ''), file=sys.stderr)
    return faults


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--tree', required=True)
    ap.add_argument('--dumps')
    ap.add_argument('--out')
    ap.add_argument('--json')
    ap.add_argument('--compare')
    ap.add_argument('--verify')
    ap.add_argument('--modlist')
    a = ap.parse_args()
    if bool(a.verify) == bool(a.dumps):
        die('give either --dumps DIR --out FILE (make the tables) or --verify FILE (check existing ones)')
    if a.dumps and not a.out:
        die('--dumps needs --out')
    if a.verify and (a.out or a.json or a.compare):
        die('--verify takes only --tree and --modlist')
    tree = a.tree.rstrip('/')
    if not os.path.isdir(os.path.join(tree, 'remastered')):
        die(f'{tree} has no remastered folder')
    modlist = a.modlist or (os.path.join(tree, 'khnmc-hd-modlist.txt') if os.path.isfile(os.path.join(tree, 'khnmc-hd-modlist.txt')) else None)
    mod = read_modlist(modlist) if modlist else None
    game, hd = tree_files(tree)
    game_lower = {g.lower() for g in game}
    print(f'hd_packages: tree {tree}: {len(game)} game files, {len(hd)} files below remastered/; provenance list: {modlist or "none"}')

    if a.verify:
        tabs, order = read_tsv(a.verify)
        faults = verify(tree, [tabs[p.lower()] for p in order], hd, mod, game_lower)
        if order != sorted(order):
            print('hd_packages: FAULT the lines are not sorted by path', file=sys.stderr)
            faults += 1
        print(f'hd_packages: {a.verify}: ' + ('verified' if not faults else 'NOT verified'))
        sys.exit(2 if faults else 0)

    pk = {}
    for name in PACKAGES:
        p = os.path.join(a.dumps, name + '.bin')
        if not os.path.isfile(p):
            die(f'{p} is missing')
        pk[name] = read_dump(p)
        print(f'hd_packages: {name}: {len(pk[name])} files, {sum(1 for e in pk[name].values() if e)} with entries, {sum(len(e) for e in pk[name].values())} entries')
    if a.json:
        with open(a.json) as f:
            js = json.load(f)
        bad = 0
        for h, lst in js.items():
            for rec in lst:
                mine = pk.get(rec[0], {}).get(h)
                theirs = [(e[0], e[2] & 0xffffffff, e[3]) for e in rec[3]]
                bad += mine != theirs
        if bad or sum(len(v) for v in js.values()) != sum(len(v) for v in pk.values()):
            die(f'{a.json} does not hold the same entries as the dumps ({bad} records differ)')
        print(f'hd_packages: {a.json}: the same {sum(len(v) for v in js.values())} records as the dumps')

    tables, seen, nowhere, low_hits, several, several_differ = [], set(), 0, 0, 0, 0
    for g in game:
        d, _, fn = g.rpartition('/')
        keys = (md5(g), md5((d + '/' if d else '') + fn.lower()))
        hit = None
        for name in PACKAGES:
            ents, k = pk[name].get(keys[0]), keys[0]
            if ents is None:
                ents, k = pk[name].get(keys[1]), keys[1]
            if ents is None:
                continue
            seen.add((name, k))
            if hit is None:
                hit = ents
                low_hits += k != keys[0]
            else:
                several += 1
                several_differ += ents != hit
        if hit is None:
            nowhere += 1
        elif hit:
            tables.append((g, hit))
    orphans = sum(1 for name in PACKAGES for h, e in pk[name].items() if e and (name, h) not in seen)
    print(f'hd_packages: {len(game) - nowhere} game files of the tree are in a package ({low_hits} only by the lower-cased file name), {nowhere} in none; '
          f'{several} are in a second package as well ({several_differ} of those with another table: the first package wins); '
          f'{orphans} package files with entries have no game file in the tree')
    faults = verify(tree, tables, hd, mod, game_lower)
    lines = [fmt_line(p, e) for p, e in tables]
    if a.compare:
        _, order = read_tsv(a.compare)
        with open(a.compare, encoding='latin-1') as f:
            old = [l.rstrip('\r\n') for l in f if l.strip() and not l.startswith('#')]
        if old != lines:
            only_old, only_new = len(set(old) - set(lines)), len(set(lines) - set(old))
            print(f'hd_packages: FAULT {a.compare} differs: {only_old} lines only there, {only_new} only here' + ('' if only_old or only_new else ' (same lines, another order)'), file=sys.stderr)
            faults += 1
        else:
            print(f'hd_packages: {a.compare}: the same {len(lines)} lines')
    if faults:
        die(f'not written: {a.out}')
    with open(a.out, 'w', encoding='latin-1', newline='\n') as f:
        f.write(HEADER)
        f.write('\n'.join(lines) + '\n')
    print(f'hd_packages: wrote {a.out}: {len(lines)} game files, {sum(len(e) for _, e in tables)} entries')


if __name__ == '__main__':
    main()
