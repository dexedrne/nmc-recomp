#!/usr/bin/env python3
"""check_ios_symbols.py : a program built with KH_IOS_PATHS must not IMPORT what an iOS app cannot use.  Fails loudly; one line.

  check_ios_symbols.py --bin khnmc-iospaths [--allow NAME ...]

Reads the undefined (imported) symbols of the linked program with `nm -u` (ELF and Mach-O; the Mach-O leading underscore and an
ELF `@GLIBC_x` version suffix are removed) and refuses:
  process creation     fork vfork system popen pclose posix_spawn posix_spawnp execl execle execlp execv execve execvp execvpe
                       fexecve daemon forkpty   (an iOS app is one process; the sandbox refuses the rest, `system` is not in the SDK)
  temporary files that are created outside the place the host gives (runtime/host_env.c: kh_tmpdir)
                       tmpfile tmpfile64 tmpnam tmpnam_r tempnam mktemp
`waitpid` is not on the list: the fatal-path selftests keep their (unreachable in such a build) parent halves, and waitpid exists
on iOS.  This is a link-level proof for THIS program on THIS host; it does not replace compiling against the iOS SDK, whose
availability attributes cover more than this list (see implementation notes, "unproven").
Python 3.6+, standard library, `nm` from the toolchain.
"""
import subprocess
import sys

FORBIDDEN = {
    'fork': 'process creation', 'vfork': 'process creation', 'system': 'process creation', 'popen': 'process creation',
    'pclose': 'process creation', 'posix_spawn': 'process creation', 'posix_spawnp': 'process creation', 'execl': 'process creation',
    'execle': 'process creation', 'execlp': 'process creation', 'execv': 'process creation', 'execve': 'process creation',
    'execvp': 'process creation', 'execvpe': 'process creation', 'fexecve': 'process creation', 'daemon': 'process creation',
    'forkpty': 'process creation',
    'tmpfile': 'temporary file outside kh_tmpdir()', 'tmpfile64': 'temporary file outside kh_tmpdir()',
    'tmpnam': 'temporary file outside kh_tmpdir()', 'tmpnam_r': 'temporary file outside kh_tmpdir()',
    'tempnam': 'temporary file outside kh_tmpdir()', 'mktemp': 'temporary file outside kh_tmpdir()',
}


def die(msg):
    sys.stderr.write('check_ios_symbols: ERROR: %s\n' % msg)
    sys.exit(1)


def main():
    argv = sys.argv[1:]
    if len(argv) < 2 or argv[0] != '--bin':
        die('usage: check_ios_symbols.py --bin FILE [--allow NAME ...]\n%s' % __doc__)
    binp = argv[1]
    allow = set()
    if len(argv) > 2:
        if argv[2] != '--allow':
            die('unexpected argument %r' % argv[2])
        allow = set(argv[3:])
    try:
        allsyms = subprocess.check_output(['nm', binp], universal_newlines=True, stderr=subprocess.DEVNULL)
        text = subprocess.check_output(['nm', '-u', binp], universal_newlines=True, stderr=subprocess.DEVNULL)
    except (OSError, subprocess.CalledProcessError) as e:
        die('nm %s: %s' % (binp, e))
    macho = any(ln.split()[-1:] == ['__mh_execute_header'] for ln in allsyms.splitlines())
    names = set()
    for ln in text.splitlines():
        p = ln.split()
        if not p:
            continue
        n = p[-1]
        if macho:
            if not n.startswith('_'):
                continue
            n = n[1:]
        n = n.split('@', 1)[0]
        names.add(n)
    if len(names) < 20:
        die('%s: only %d imported symbols read: is this the linked program?' % (binp, len(names)))
    # the switch itself: such a build defines the conf reader's entry and says so in its symbol table
    if 'kh_host_env_selftest' not in allsyms:
        die('%s has no kh_host_env_selftest symbol: not a khnmc program' % binp)
    bad = sorted(n for n in names if n in FORBIDDEN and n not in allow)
    if bad:
        die('%s imports %s' % (binp, ', '.join('%s (%s)' % (n, FORBIDDEN[n]) for n in bad)))
    print('check_ios_symbols: %s: %s, %d imported symbols, none of the %d that an iOS app cannot use (fork, system, popen, exec*, posix_spawn, tmpfile, ...)%s'
          % (binp, 'Mach-O' if macho else 'ELF', len(names), len(FORBIDDEN), '; allowed by request: ' + ' '.join(sorted(allow)) if allow else ''))


if __name__ == '__main__':
    main()
