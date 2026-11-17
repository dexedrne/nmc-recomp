#!/usr/bin/env python3
"""apply_gummi_overlay.py [--in-prog SHA] [--in-wm00 SHA] [--langs us,fr,gr,it,sp] TREE EDITED_WM00_ARD EDITED_00PROGRESS
- put the gummi-removal data edits into a baked data tree.

The gummi ship removal is a PORT deviation the owner asked for ("not even in the game"; docs/gummi/GUMMI-REMOVAL.md): 00progress.bin
WLDF entry 53 block cmd 5 -> 14 (the TWTNW unlock no longer re-blocks Twilight Town) and wm00.ard without the m_51 gummi spawn.  Every
bake from vanilla + mod puts back the mod's unedited 00progress.bin and the unedited wm00.ard, so this runs after EVERY bake
(docs/MOD-UPDATES.md step "gummi"; tools/mod_update_check.py lists it whenever it lists the bake).

Safety: each target is replaced only if its current SHA-256 equals the INPUT hash the edit was built from (wm00.ard: IN_WM00,
00progress.bin: IN_PROG) or already equals the edited file (idempotent).  IN_PROG is the sha256 of the pinned mod version's
00progress.bin and IN_WM00 that of the vanilla wm00.ard (the pinned mod ships none); tools/mod_update_check.py --write-fingerprint
checks both against the version it pins, so a new mod version cannot be re-pinned while they are stale.  For a new version give the
new input hashes with --in-prog / --in-wm00 (after making the edited files from that version's inputs with
tools/gummi/make_gummi_removal.py).  The edited files are game-derived: never committed.  wm00.ard is patched in every language folder
of --langs (default all five, as data-full has it).

Exit status: 0 when every target is edited or already edited; 1 when one was REFUSED (unexpected content, not touched) or is absent
(the tree would keep that file unedited: never a silent success); 2 on a usage error.
(Task 34 review: the repository version printed REFUSED and still exited 0, and patched with fixed input hashes only.)
"""
import argparse, hashlib, os, shutil, sys

IN_WM00 = '32a8a76a400f871c60a395b503aa10feb66b796f5901f8bcfc576021eae26598'
IN_PROG = 'cdba15a9241093f6f407fda869380ee99ce41a59a8f57484734a979911d70dbb'
LANGS = ('us', 'fr', 'gr', 'it', 'sp')


def sha(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for blk in iter(lambda: f.read(1 << 22), b''):
            h.update(blk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('tree')
    ap.add_argument('wm00_edited')
    ap.add_argument('progress_edited')
    ap.add_argument('--in-prog', default=IN_PROG, help='sha256 of the 00progress.bin the edit was made from (default: the pinned mod\'s)')
    ap.add_argument('--in-wm00', default=IN_WM00, help='sha256 of the wm00.ard the edit was made from (default: the vanilla file)')
    ap.add_argument('--langs', default=','.join(LANGS), help='language folders of ard/ whose wm00.ard is patched')
    a = ap.parse_args()
    for p in (a.wm00_edited, a.progress_edited):
        if not os.path.isfile(p):
            print('apply_gummi_overlay: no such file: %s' % p, file=sys.stderr)
            return 2
    if not os.path.isdir(a.tree):
        print('apply_gummi_overlay: no such tree: %s' % a.tree, file=sys.stderr)
        return 2
    langs = [x for x in a.langs.split(',') if x]
    jobs = [('ard/%s/wm00.ard' % l, a.wm00_edited, a.in_wm00) for l in langs] + [('00progress.bin', a.progress_edited, a.in_prog)]
    new_hash = {a.wm00_edited: sha(a.wm00_edited), a.progress_edited: sha(a.progress_edited)}
    bad = 0
    done = 0
    for rel, new, inh in jobs:
        p = os.path.join(a.tree, rel)
        if not os.path.isfile(p):
            print('ABSENT (the tree has no %s: not edited):' % rel, rel)
            bad += 1
            continue
        cur = sha(p)
        if cur == new_hash[new]:
            print('already edited:', rel)
            done += 1
        elif cur == inh:
            tmp = p + '.tmp'
            shutil.copyfile(new, tmp)
            if os.path.islink(p) or os.stat(p).st_nlink > 1:   # hardlinked from the vanilla tree: break the link
                os.remove(p)
            os.replace(tmp, p)
            print('edited:', rel)
            done += 1
        else:
            print('REFUSED (unexpected content %s.., the edit was made from %s..: not touched):' % (cur[:12], inh[:12]), rel)
            bad += 1
    print('apply_gummi_overlay: %d of %d targets edited or already edited, %d refused or absent' % (done, len(jobs), bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
