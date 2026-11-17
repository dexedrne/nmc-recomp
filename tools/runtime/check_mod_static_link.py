#!/usr/bin/env python3
"""Link check for the mod static-edit replacements (Task 12).  Fails loudly; prints one summary line on success.

  check_mod_static_link.py --bin khnmc --obj obj --funcs "RVA ..." --chained "RVA ..."

For every function in --funcs:
  * the linked program has a STRONG f_<rva> (a weak one would mean the vanilla archive body was linked instead of the override)
    and a distinct orig_f_<rva> (the vanilla lifted body, kept reachable);
  * not chained: obj/mod_static_lifted.o defines f_<rva> and no runtime object does (so the strong f_<rva> is the re-lifted body);
  * chained (a runtime hook owns f_<rva>): obj/mod_static_lifted.o defines mod_f_<rva>, exactly the object that defines f_<rva>
    references mod_f_<rva>;
  * no runtime object calls orig_f_<rva> directly (only the {rva, replacement, original} table in mod_static_lifted.o may name it).
"""
import sys, os, subprocess, argparse, glob

def die(msg):
    sys.stderr.write('check_mod_static_link: ERROR: %s\n' % msg); sys.exit(1)

def nm(path, extra=()):
    out = {}
    for ln in subprocess.check_output(['nm', *extra, path], text=True).splitlines():
        p = ln.split()
        if len(p) == 3: out[p[2]] = (p[1], int(p[0], 16))
        elif len(p) == 2: out[p[1]] = (p[0], None)
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', required=True); ap.add_argument('--obj', required=True); ap.add_argument('--funcs', required=True); ap.add_argument('--chained', default='')
    a = ap.parse_args()
    funcs = a.funcs.split(); chained = set(a.chained.split())
    b = nm(a.bin)
    lifted = os.path.join(a.obj, 'mod_static_lifted.o'); lo = nm(lifted)
    objs = {p: nm(p) for p in sorted(glob.glob(os.path.join(a.obj, '*.o'))) if os.path.abspath(p) != os.path.abspath(lifted)}
    for r in funcs:
        f, o, m = 'f_' + r, 'orig_f_' + r, 'mod_f_' + r
        if f not in b or o not in b: die('%s or %s missing from %s' % (f, o, a.bin))
        if b[f][0] != 'T': die('%s is %r in %s, expected a strong definition (T): the override was not linked' % (f, b[f][0], a.bin))
        if b[o][0] not in 'Tt' or b[o][1] == b[f][1]: die('%s is not a distinct function (%r)' % (o, b[o]))
        users = [p for p, s in objs.items() if s.get(o, ('', 0))[0] == 'U']
        if users: die('%s is called directly by %s: the vanilla body must not be reached by runtime code' % (o, users))
        if lo.get(o, ('',))[0] != 'U': die('%s is not referenced by the table in %s' % (o, lifted))
        if r in chained:
            if lo.get(m, ('',))[0] != 'T' or f in lo and lo[f][0] == 'T': die('%s: %s must define %s and not %s' % (r, lifted, m, f))
            owners = [p for p, s in objs.items() if s.get(f, ('',))[0] == 'T']
            refs = [p for p, s in objs.items() if s.get(m, ('',))[0] == 'U']
            if len(owners) != 1 or refs != owners: die('%s: the runtime hook that defines %s (%s) must be the one object that forwards to %s (%s)' % (r, f, owners, m, refs))
            if m not in b or b[m][1] in (b[f][1], b[o][1]): die('%s: %s is not a distinct function in %s' % (r, m, a.bin))
        else:
            if lo.get(f, ('',))[0] != 'T': die('%s does not define %s' % (lifted, f))
            if any(s.get(f, ('',))[0] == 'T' for s in objs.values()): die('%s is also defined by a runtime object' % f)
    print('check_mod_static_link: %d functions: strong f_<rva> + distinct orig_f_<rva> for all, %d chained through their runtime hook to mod_f_<rva>, no direct orig_f_ callers'
          % (len(funcs), len(chained)))

if __name__ == '__main__':
    main()
