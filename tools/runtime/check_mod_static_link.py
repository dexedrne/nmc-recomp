#!/usr/bin/env python3
"""Link check for the mod static-edit replacements (Task 12).  Fails loudly; prints one summary line on success.

  check_mod_static_link.py --bin khnmc --obj obj --funcs "RVA ..." --chained "RVA ..." [--replaced "RVA ..."] [--choice "RVA ..."]
                            [--dll-replaces "RVA ..."]

For every function in --funcs:
  * the linked program has a STRONG f_<rva> (a weak one would mean the vanilla archive body was linked instead of the override)
    and a distinct orig_f_<rva> (the vanilla lifted body, kept reachable);
  * not chained: obj/mod_static_lifted.o defines f_<rva> and no runtime object does (so the strong f_<rva> is the re-lifted body);
  * chained (a runtime hook owns f_<rva>): obj/mod_static_lifted.o defines mod_f_<rva>, exactly the object that defines f_<rva>
    references mod_f_<rva>;
  * replaced (Task 23; a subset of --chained as far as the lifter is concerned: the body is emitted as mod_f_<rva>): the mod DLL's
    detour on this function never calls its trampoline, so on the PC neither the vanilla nor the patched body runs.  The runtime hook
    that defines f_<rva> must be a full replacement: exactly one runtime object defines f_<rva> and NO runtime object references
    mod_f_<rva> (a forward to the re-lifted body would run code that is dead on the PC).  mod_f_<rva> stays linked as an unused
    reference, so that every static edit keeps its re-lifted block and its differential test;
  * choice (Task 23; also emitted as mod_f_<rva>): a patch that an ini value switches (KH2Moose.dll).  The runtime hook that defines
    f_<rva> forwards to mod_f_<rva> when the patch is on and to orig_f_<rva> when it is off: that one object, and only it, must
    reference BOTH symbols;
  * no runtime object calls orig_f_<rva> directly (only the {rva, replacement, original} table in mod_static_lifted.o may name it;
    the one exception is the owner of a `choice` function).

--dll-replaces (review of Task 23) is independent of --funcs: the functions on which KH2Tweaks_Refactor.dll attaches a detour whose
handler never reaches its trampoline (the REPLACES rows of the 35-detour table in implementation notes).  On the PC no state runs the
vanilla body of such a function, so the port must not have a path into it either: exactly one runtime object defines a strong
f_<rva>, and that object does not reference orig_f_<rva>.  (Other objects may: the selftests call some originals for comparison.)
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
    ap.add_argument('--replaced', default=''); ap.add_argument('--choice', default=''); ap.add_argument('--dll-replaces', default='')
    a = ap.parse_args()
    funcs = a.funcs.split(); chained = set(a.chained.split()); replaced = set(a.replaced.split()); choice = set(a.choice.split())
    for k, st in (('--chained', chained), ('--replaced', replaced), ('--choice', choice)):
        if st - set(funcs): die('%s names functions that are not in --funcs: %s' % (k, sorted(st - set(funcs))))
    if (chained & replaced) or (chained & choice) or (replaced & choice): die('--chained, --replaced and --choice must be disjoint')
    b = nm(a.bin)
    lifted = os.path.join(a.obj, 'mod_static_lifted.o'); lo = nm(lifted)
    objs = {p: nm(p) for p in sorted(glob.glob(os.path.join(a.obj, '*.o'))) if os.path.abspath(p) != os.path.abspath(lifted)}
    dll_replaces = a.dll_replaces.split()
    for r in dll_replaces:
        f, o = 'f_' + r, 'orig_f_' + r
        if b.get(f, ('',))[0] != 'T': die('%s is %r in %s, expected a strong definition (T): the port of the DLL handler was not linked' % (f, b.get(f), a.bin))
        owners = [p for p, s in objs.items() if s.get(f, ('',))[0] == 'T']
        if len(owners) != 1: die('%s: exactly one runtime object must define %s (found %s)' % (r, f, owners))
        if objs[owners[0]].get(o, ('',))[0] == 'U':
            die('%s: the mod DLL replaces this function outright (its handler never reaches the trampoline), but its port in %s references %s: '
                'a forward to the vanilla body is a path the PC game does not have' % (r, owners[0], o))
    for r in funcs:
        f, o, m = 'f_' + r, 'orig_f_' + r, 'mod_f_' + r
        if f not in b or o not in b: die('%s or %s missing from %s' % (f, o, a.bin))
        if b[f][0] != 'T': die('%s is %r in %s, expected a strong definition (T): the override was not linked' % (f, b[f][0], a.bin))
        if b[o][0] not in 'Tt' or b[o][1] == b[f][1]: die('%s is not a distinct function (%r)' % (o, b[o]))
        users = [p for p, s in objs.items() if s.get(o, ('', 0))[0] == 'U']
        if r in choice:
            owners = [p for p, s in objs.items() if s.get(f, ('',))[0] == 'T']
            mrefs = [p for p, s in objs.items() if s.get(m, ('',))[0] == 'U']
            if lo.get(m, ('',))[0] != 'T' or f in lo and lo[f][0] == 'T': die('%s: %s must define %s and not %s' % (r, lifted, m, f))
            if len(owners) != 1 or users != owners or mrefs != owners:
                die('%s (ini-switched patch): the one runtime object that defines %s (%s) must reference both %s (%s) and %s (%s)' % (r, f, owners, m, mrefs, o, users))
            if m not in b or b[m][1] in (b[f][1], b[o][1]): die('%s: %s is not a distinct function in %s' % (r, m, a.bin))
            if lo.get(o, ('',))[0] != 'U': die('%s is not referenced by the table in %s' % (o, lifted))
            continue
        if users: die('%s is called directly by %s: the vanilla body must not be reached by runtime code' % (o, users))
        if lo.get(o, ('',))[0] != 'U': die('%s is not referenced by the table in %s' % (o, lifted))
        if r in replaced:
            if lo.get(m, ('',))[0] != 'T' or f in lo and lo[f][0] == 'T': die('%s: %s must define %s and not %s' % (r, lifted, m, f))
            owners = [p for p, s in objs.items() if s.get(f, ('',))[0] == 'T']
            refs = [p for p, s in objs.items() if s.get(m, ('',))[0] == 'U']
            if len(owners) != 1: die('%s: exactly one runtime object must define %s (found %s)' % (r, f, owners))
            if refs: die('%s: the mod DLL replaces this function outright (its detour never calls the trampoline), but %s references %s: the re-lifted body must stay unused' % (r, refs, m))
            if m not in b or b[m][1] in (b[f][1], b[o][1]): die('%s: %s is not a distinct function in %s' % (r, m, a.bin))
        elif r in chained:
            if lo.get(m, ('',))[0] != 'T' or f in lo and lo[f][0] == 'T': die('%s: %s must define %s and not %s' % (r, lifted, m, f))
            owners = [p for p, s in objs.items() if s.get(f, ('',))[0] == 'T']
            refs = [p for p, s in objs.items() if s.get(m, ('',))[0] == 'U']
            if len(owners) != 1 or refs != owners: die('%s: the runtime hook that defines %s (%s) must be the one object that forwards to %s (%s)' % (r, f, owners, m, refs))
            if m not in b or b[m][1] in (b[f][1], b[o][1]): die('%s: %s is not a distinct function in %s' % (r, m, a.bin))
        else:
            if lo.get(f, ('',))[0] != 'T': die('%s does not define %s' % (lifted, f))
            if any(s.get(f, ('',))[0] == 'T' for s in objs.values()): die('%s is also defined by a runtime object' % f)
    print('check_mod_static_link: %d functions: strong f_<rva> + distinct orig_f_<rva> for all, %d chained through their runtime hook to mod_f_<rva>, '
          '%d replaced outright by their runtime hook (mod_f_<rva> linked but unreferenced), %d ini-switched (the hook picks mod_f_ or orig_f_), no other orig_f_ callers; '
          '%d DLL full replacements whose port does not reference orig_f_<rva>'
          % (len(funcs), len(chained), len(replaced), len(choice), len(dll_replaces)))

if __name__ == '__main__':
    main()
