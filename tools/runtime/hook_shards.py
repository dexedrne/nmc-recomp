#!/usr/bin/env python3
"""hook_shards.py : source-level function hooks for linkers without objcopy / --wrap (Mach-O), from the Makefile's ONE hook list.

The Linux build weakens `f_<rva>` in the prebuilt archive and adds `orig_f_<rva>` aliases with GNU objcopy
(tools/alias_file_hooks.py), and links seven probes with `-Wl,--wrap`.  Mach-O has no symbol aliases, no objcopy and no --wrap, so
this tool writes a COPY of the lifted shards (never edits lifted/ in place) in which

  * the DEFINITION of every hooked function is renamed `orig_f_<rva>`; every call site (same shard or not) and gen/kh_table.c keep
    `f_<rva>`, which is now undefined in the lifted code and binds to the runtime's strong definition;
  * the DEFINITION of every wrapped function is renamed `__real_f_<rva>`, and the call sites INSIDE its own shard call
    `__real_f_<rva>` directly: exactly what `ld --wrap` does (it redirects undefined references only, so a call from the defining
    translation unit reaches the real body; runtime/gxwrap.c says so).  References from other shards and from kh_table.c keep
    `f_<rva>` and reach `__wrap_f_<rva>` through the generated forwarder.

  shards   <src-dir> <out-dir> --hooks RVA... --wrap RVA...
        writes every shard_*.c of src-dir to out-dir (changed ones rewritten, the others copied) plus out-dir/hooks.manifest.
        Every RVA must be defined in exactly one shard, at a line `void f_<rva>(KCtx *g){`; anything else is an error.
  forwarders <out.c> --nm <file with `nm` output of the runtime objects> --hooks RVA... --wrap RVA...
        writes the forwarders the link needs:  wrapped RVA:  void f_X(KCtx*g){ __wrap_f_X(g); }
        hooked RVA for which the runtime objects define NO f_X (on Linux the weak lifted body wins there):
                                               void f_X(KCtx*g){ orig_f_X(g); }      and a NOTE on stderr naming each one.
        A runtime `f_X` that is not in the hook list is left to the linker (duplicate symbol: loud).
The line count of every shard is unchanged.  Python 3.6+, standard library only.
"""
import os
import re
import sys


def split_lists(args):
    out = {'--hooks': [], '--wrap': [], '--nm': []}
    cur = None
    rest = []
    for a in args:
        if a in out:
            cur = a
        elif cur is None:
            rest.append(a)
        else:
            out[cur].append(a)
    for k in ('--hooks', '--wrap'):
        for r in out[k]:
            if not re.fullmatch(r'[0-9a-f]+', r):
                sys.exit('hook_shards: bad rva %r in %s' % (r, k))
        if len(set(out[k])) != len(out[k]):
            sys.exit('hook_shards: duplicate rva in %s' % k)
    both = set(out['--hooks']) & set(out['--wrap'])
    if both:
        sys.exit('hook_shards: rva both hooked and wrapped: %s' % ' '.join(sorted(both)))
    return rest, out['--hooks'], out['--wrap'], out['--nm']


def do_shards(src, dst, hooks, wraps):
    os.makedirs(dst, exist_ok=True)
    names = sorted(n for n in os.listdir(src) if re.fullmatch(r'shard_\d+\.c', n))
    if not names:
        sys.exit('hook_shards: no shard_*.c in %s' % src)
    want = {('f_' + r).encode(): ('orig_f_' + r).encode() for r in hooks}
    want.update({('f_' + r).encode(): ('__real_f_' + r).encode() for r in wraps})
    wrapped = {('f_' + r).encode() for r in wraps}
    defre = re.compile(rb'^void (f_[0-9a-f]+)\(KCtx \*g\)\{', re.M)
    found = {}
    manifest = []
    changed = 0
    for n in names:
        with open(os.path.join(src, n), 'rb') as f:
            s = f.read()
        mine = [d for d in defre.findall(s) if d in want]
        out = s
        for d in mine:
            if d in found:
                sys.exit('hook_shards: %s is defined in %s and in %s' % (d.decode(), found[d], n))
            found[d] = n
            new = want[d]
            head = b'\nvoid ' + d + b'(KCtx *g){'
            if out.count(head) != 1:
                sys.exit('hook_shards: %s: expected exactly one definition line of %s' % (n, d.decode()))
            calls = len(re.findall(rb'\b' + d + rb'\(g\)', out))
            # the declaration keeps same-shard callers compiling whether or not the lifter emitted one; same line: line numbers stay
            out = out.replace(head, b'\nvoid ' + d + b'(KCtx *g); void ' + new + b'(KCtx *g){')
            if d in wrapped and calls:
                out = re.sub(rb'\b' + d + rb'\(g\)', new + b'(g)', out)
                decl = b'\nvoid ' + d + b'(KCtx *g);\n'         # the lifter's forward declaration: the callers precede the definition
                if out.count(decl) != 1:
                    sys.exit('hook_shards: %s: %s has same-shard callers but not exactly one forward declaration line' % (n, d.decode()))
                out = out.replace(decl, b'\nvoid ' + d + b'(KCtx *g); void ' + new + b'(KCtx *g);\n')
            manifest.append('%s %s -> %s  same-shard call sites: %d (%s)' % (
                n, d.decode(), new.decode(), calls,
                'call the real body, as with ld --wrap' if d in wrapped else 'bind to the runtime hook'))
        if out.count(b'\n') != s.count(b'\n'):
            sys.exit('hook_shards: %s: line count changed' % n)
        path = os.path.join(dst, n)
        old = None
        if os.path.exists(path):
            with open(path, 'rb') as f:
                old = f.read()
        if old != out:                       # keep timestamps of unchanged outputs: make then recompiles nothing
            with open(path + '.tmp', 'wb') as f:
                f.write(out)
            os.replace(path + '.tmp', path)
        changed += out != s
    missing = sorted(k.decode() for k in want if k not in found)
    if missing:
        sys.exit('hook_shards: no definition found for: %s' % ' '.join(missing))
    for n in os.listdir(dst):                # a shard that no longer exists in the source must not be linked
        if re.fullmatch(r'shard_\d+\.c', n) and n not in names:
            sys.exit('hook_shards: stale %s in %s (not in %s): remove the output directory' % (n, dst, src))
    text = '\n'.join(sorted(manifest)) + '\n'
    mpath = os.path.join(dst, 'hooks.manifest')
    old = open(mpath).read() if os.path.exists(mpath) else None
    if old != text:
        with open(mpath, 'w') as f:
            f.write(text)
    print('hook_shards: %d shards, %d rewritten; %d hooked definitions -> orig_f_*, %d wrapped -> __real_f_*' % (
        len(names), changed, len(hooks), len(wraps)))


def do_forwarders(out_c, hooks, wraps, nmfiles):
    defined = set()
    for p in nmfiles:
        with open(p) as f:
            for line in f:
                fld = line.split()
                if len(fld) >= 3 and fld[-2] == 'T':          # global text definitions only: a static one does not satisfy the linker
                    sym = fld[-1]
                    defined.add(sym)
                    if sym.startswith('_'):                   # Mach-O spells C symbol f_X as _f_X
                        defined.add(sym[1:])
    if not nmfiles:
        sys.exit('hook_shards: forwarders need --nm <file>')
    if not defined:
        sys.exit('hook_shards: the nm output names no global text symbol at all (wrong file?)')
    lines = ['/* generated by tools/hook_shards.py forwarders: do not edit */', '#include "khrt.h"']
    weak = []
    for r in hooks:
        if 'f_' + r not in defined:
            weak.append(r)
            lines.append('void orig_f_%s(KCtx *g); void f_%s(KCtx *g){ orig_f_%s(g); }   /* hooked, but the runtime defines no f_%s: the lifted body runs */' % (r, r, r, r))
    for r in wraps:
        if '__wrap_f_' + r not in defined:
            sys.exit('hook_shards: wrapped f_%s but the runtime objects define no __wrap_f_%s' % (r, r))
        lines.append('void __wrap_f_%s(KCtx *g); void f_%s(KCtx *g){ __wrap_f_%s(g); }' % (r, r, r))
    text = '\n'.join(lines) + '\n'
    old = open(out_c).read() if os.path.exists(out_c) else None
    if old != text:
        with open(out_c, 'w') as f:
            f.write(text)
    if weak:
        sys.stderr.write('hook_shards: NOTE: %d hooked function(s) have no runtime definition; the lifted body is used through a generated forwarder: %s\n'
                         % (len(weak), ' '.join('f_' + r for r in weak)))
    print('hook_shards: forwarders: %d wrap, %d hooked-without-runtime-definition' % (len(wraps), len(weak)))


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ('shards', 'forwarders'):
        sys.exit(__doc__)
    rest, hooks, wraps, nmfiles = split_lists(sys.argv[2:])
    if sys.argv[1] == 'shards':
        if len(rest) != 2:
            sys.exit('usage: hook_shards.py shards <src-dir> <out-dir> --hooks RVA... --wrap RVA...')
        do_shards(rest[0], rest[1], hooks, wraps)
    else:
        if len(rest) != 1:
            sys.exit('usage: hook_shards.py forwarders <out.c> --nm <nm-output> --hooks RVA... --wrap RVA...')
        do_forwarders(rest[0], hooks, wraps, nmfiles)


if __name__ == '__main__':
    main()
