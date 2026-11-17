#!/usr/bin/env python3
"""check_hook_link.py : link check for EVERY function hook of the build, on ELF and on Mach-O.  Fails loudly; one summary line.

  check_hook_link.py --bin khnmc --objs a.o b.o ... --hooks RVA... --wrap RVA... [--forwarders hook_forwarders.o] [--strict]

--objs = every object that is linked directly (runtime objects, lifter-rt objects, other lifted objects such as a re-lifted
override file, the generated forwarder object): everything except the archive of lifted shards.

For every RVA in --hooks (the expanded FILE_HOOKS list of the Makefile):
  * at most ONE of --objs defines a strong `f_<rva>` (two would be a duplicate-symbol link error on Mach-O and on ELF; reported here
    with the object names);
  * the linked program has `f_<rva>` and a DISTINCT function `orig_f_<rva>` (the vanilla lifted body, kept reachable), and
    `f_<rva>` is a strong symbol there when an object defines one;
  * when NO object defines `f_<rva>` the vanilla lifted body runs: through the weak archive symbol on ELF (`f_<rva>` is then weak
    and the same address as `orig_f_<rva>`), through the generated forwarder with HOOKMODE=rename (--forwarders names that object,
    whose `f_<rva>` does not count as a replacement).  That is a NOTE naming each such RVA, or an error with --strict.
For every RVA in --wrap: exactly one object defines `__wrap_f_<rva>`, and the program has `__wrap_f_<rva>` and the real body
(`__real_f_<rva>` with renamed definitions, `f_<rva>` with ld --wrap) as distinct functions.
Python 3.6+, standard library, `nm` from the toolchain.
"""
import subprocess
import sys


def die(msg):
    sys.stderr.write('check_hook_link: ERROR: %s\n' % msg)
    sys.exit(1)


def nm_rows(path):
    try:
        text = subprocess.check_output(['nm', path], universal_newlines=True, stderr=subprocess.DEVNULL)
    except (OSError, subprocess.CalledProcessError) as e:
        die('nm %s: %s' % (path, e))
    rows = []
    for ln in text.splitlines():
        p = ln.split()
        if len(p) == 3:
            rows.append((p[2], p[1], int(p[0], 16)))
        elif len(p) == 2:
            rows.append((p[1], p[0], None))
    return rows


def table(rows, macho):
    """symbol -> (type, value or None); Mach-O C symbols lose their leading underscore so both formats read alike"""
    out = {}
    for n, t, v in rows:
        if macho:
            if not n.startswith('_'):
                continue
            n = n[1:]
        if n in out and out[n][0] in 'TW' and t not in 'TW':
            continue
        out[n] = (t, v)
    return out


def args():
    a = {'--bin': [], '--objs': [], '--hooks': [], '--wrap': [], '--forwarders': []}
    strict = False
    cur = None
    for x in sys.argv[1:]:
        if x == '--strict':
            strict = True
        elif x in a:
            cur = x
        elif cur is None:
            die('unexpected argument %r\n%s' % (x, __doc__))
        else:
            a[cur].append(x)
    if len(a['--bin']) != 1 or not a['--objs'] or not a['--hooks']:
        die('need --bin FILE, --objs FILES and --hooks RVAS\n%s' % __doc__)
    return a['--bin'][0], a['--objs'], a['--hooks'], a['--wrap'], a['--forwarders'], strict


def main():
    binp, objs, hooks, wraps, fwd, strict = args()
    if len(set(hooks)) != len(hooks):
        die('duplicate rva in --hooks: %s' % ' '.join(sorted(r for r in set(hooks) if hooks.count(r) > 1)))
    rows = nm_rows(binp)
    macho = any(n == '__mh_execute_header' for n, _, _ in rows)      # Mach-O executable: C symbol f_X is spelled _f_X
    b = table(rows, macho)
    syms = {p: table(nm_rows(p), macho) for p in objs}
    fwd = set(fwd)
    for p in fwd:
        if p not in syms:
            die('--forwarders %s is not one of --objs' % p)
    unreplaced = []
    for r in hooks:
        f, o = 'f_' + r, 'orig_f_' + r
        owners = [p for p, s in syms.items() if s.get(f, ('',))[0] == 'T' and p not in fwd]
        fowners = [p for p in fwd if syms[p].get(f, ('',))[0] == 'T']
        if len(owners) > 1:
            die('%s has %d strong definitions: %s' % (f, len(owners), ' '.join(owners)))
        if owners and fowners:
            die('%s is defined by %s and also by the generated forwarder %s (stale forwarder file?)' % (f, owners[0], fowners[0]))
        if f not in b or o not in b:
            die('%s or %s is missing from %s' % (f, o, binp))
        if b[o][0] not in 'Tt':
            die('%s is not a function in %s (%r)' % (o, binp, b[o]))
        if owners:
            if b[f][0] != 'T':
                die('%s is %r in %s although %s defines it: the replacement was not linked' % (f, b[f][0], binp, owners[0]))
            if b[f][1] == b[o][1]:
                die('%s and %s are the same address in %s: the vanilla body was linked as the hook' % (f, o, binp))
        else:
            if fowners:
                if b[f][1] == b[o][1]:
                    die('%s: forwarder and %s share an address in %s' % (f, o, binp))
            elif not (b[f][0] in 'Ww' and b[f][1] == b[o][1]):
                die('%s has no definition in --objs, yet in %s it is %r at %s (orig at %s): expected the weak vanilla body or a forwarder'
                    % (f, binp, b[f][0], b[f][1] and hex(b[f][1]), b[o][1] and hex(b[o][1])))
            unreplaced.append(r)
    for r in wraps:
        w, f, real = '__wrap_f_' + r, 'f_' + r, '__real_f_' + r
        owners = [p for p, s in syms.items() if s.get(w, ('',))[0] == 'T']
        if len(owners) != 1:
            die('%s must be defined by exactly one object, found %d: %s' % (w, len(owners), ' '.join(owners)))
        if w not in b:
            die('%s is missing from %s' % (w, binp))
        body = real if real in b else f                      # renamed definition, or ld --wrap (the real body keeps the name f_X)
        if body not in b or b[body][0] not in 'Tt' or b[body][1] == b[w][1]:
            die('%s: no distinct real body (%s) in %s' % (w, body, binp))
        if r in hooks:
            die('f_%s is both hooked and wrapped' % r)
    if unreplaced:
        msg = '%d hooked function(s) have NO replacement in the link; the vanilla lifted body runs: %s' % (len(unreplaced), ' '.join('f_' + r for r in unreplaced))
        if strict:
            die(msg)
        sys.stderr.write('check_hook_link: NOTE: %s\n' % msg)
    print('check_hook_link: %d hooks: one strong f_<rva> + a distinct orig_f_<rva> for %d, %d left to the vanilla body; %d wrap probes with a distinct real body (%s, %d objects)'
          % (len(hooks), len(hooks) - len(unreplaced), len(unreplaced), len(wraps), binp, len(objs)))


if __name__ == '__main__':
    main()
