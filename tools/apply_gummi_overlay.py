#!/usr/bin/env python3
"""apply_gummi_overlay.py TREE EDITED_WM00_ARD EDITED_00PROGRESS - put the gummi-removal data edits into a baked data tree.

Safety: each target is replaced only if its current SHA-256 equals the INPUT hash the edit was built from (wm00.ard: 32a8a76a...,
00progress.bin: cdba15a9...) or already equals the edited file (idempotent). The edited files are produced by tools/gummi/make_gummi_removal.py
from the owner's own extraction and are game-derived: they are never committed. wm00.ard is patched for every language folder whose copy
is byte-identical to the input (ard/{us,fr,gr,it,sp}/wm00.ard).
"""
import hashlib, os, shutil, sys
IN_WM00 = '32a8a76a400f871c60a395b503aa10feb66b796f5901f8bcfc576021eae26598'
IN_PROG = 'cdba15a9241093f6f407fda869380ee99ce41a59a8f57484734a979911d70dbb'
def sha(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()
tree, wm_new, pr_new = sys.argv[1:4]
jobs = [(f'ard/{l}/wm00.ard', wm_new, IN_WM00) for l in ('us', 'fr', 'gr', 'it', 'sp')] + [('00progress.bin', pr_new, IN_PROG)]
new_hash = {wm_new: sha(wm_new), pr_new: sha(pr_new)}
for rel, new, inh in jobs:
    p = os.path.join(tree, rel)
    if not os.path.isfile(p):
        print('skip (absent):', rel); continue
    cur = sha(p)
    if cur == new_hash[new]:
        print('already edited:', rel)
    elif cur == inh:
        tmp = p + '.tmp'; shutil.copyfile(new, tmp)
        if os.path.islink(p) or os.stat(p).st_nlink > 1:   # hardlinked from the vanilla tree: break the link
            os.remove(p)
        os.replace(tmp, p); print('edited:', rel)
    else:
        print('REFUSED (unexpected content, not touched):', rel, cur[:12])
