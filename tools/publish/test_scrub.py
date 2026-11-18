#!/usr/bin/env python3
"""tools/publish/test_scrub.py : checks of scrub.py on throwaway repositories in a new temporary directory (removed at the end).

  python3 tools/publish/test_scrub.py [SCRUB]     SCRUB = the scrub.py to test (default: the one next to this file)

Exit 0 when every check passed, 1 otherwise; every check prints `ok` or `FAIL`.  The fixture repositories are written with
`git fast-import` under a neutral test identity, with HOME pointed at the temporary directory and no system or global git
configuration read, so no hook and no setting of the caller's runs.  Nothing outside the temporary directory is touched.
The marker below is a made-up word, not a real rule.
"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRUB = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(HERE, 'scrub.py')
MARK = 'ZQX_PRIVATE_MARKER'
fails = checks = 0


def check(cond, what):
    global fails, checks
    checks += 1
    if not cond:
        fails += 1
    print('%s  %s' % ('ok  ' if cond else 'FAIL', what))


def make_repo(root, name, files, message='a commit\n'):
    """One commit on main holding `files` ({path: bytes}); returns the repository's path."""
    repo = os.path.join(root, name)
    subprocess.run(['git', 'init', '-q', '-b', 'main', repo], check=True, env=ENV)
    msg = message.encode()
    out = [b'commit refs/heads/main\n',
           b'author Test <331735862+dexedrne@users.noreply.github.com> 1700000000 +0000\n',
           b'committer Test <331735862+dexedrne@users.noreply.github.com> 1700000000 +0000\n',
           b'data %d\n' % len(msg), msg]
    for path, data in files.items():
        out += [b'M 100644 inline %s\n' % path.encode(), b'data %d\n' % len(data), data, b'\n']
    subprocess.run(['git', '-C', repo, 'fast-import', '--quiet'], input=b''.join(out), check=True, env=ENV)
    return repo


def scan(repo):
    p = subprocess.run([sys.executable, SCRUB, '--rules', RULES, '--scan', repo], env=ENV,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode('utf-8', 'replace')


with tempfile.TemporaryDirectory(prefix='test-scrub-') as tmp:
    ENV = dict(os.environ, HOME=tmp, GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL=os.devnull, LC_ALL='C')
    RULES = os.path.join(tmp, 'rules.tsv')
    with open(RULES, 'w', encoding='utf-8') as f:
        f.write('# test rules\nliteral\t%s\tREDACTED\nforbid\t%s\n' % (MARK, MARK))
    m = MARK.encode()
    clean_bin = b'\0\x01\x02 clean bytes \xff\xfe' * 10

    # controls: what the scan found before the fix too
    rc, out = scan(make_repo(tmp, 'clean', {'a.txt': b'hello\n', 'b.dat': clean_bin}))
    check(rc == 0 and 'scan clean' in out, 'a clean repository with one clean binary blob passes')
    check('(1 binary' in out, 'the summary line counts the binary blob: ' + out.strip())
    rc, out = scan(make_repo(tmp, 'text', {'a.txt': b'x ' + m + b' y\n'}))
    check(rc == 1 and 'contents of a.txt' in out, 'the marker in a text file is a hit')
    rc, out = scan(make_repo(tmp, 'msg', {'a.txt': b'hello\n'}, message='fix for %s\n' % MARK))
    check(rc == 1 and 'commit message' in out, 'the marker in a commit message is a hit')

    # review 2026-11-17 (n=0): a blob with a NUL in its first 8 KiB was stored as clean without being searched
    cases = [
        ('NUL then the marker (the review case)', b'\0' + m),
        ('marker after 9 KiB of a binary blob', b'\0' + b'x' * 9000 + m + b'\0'),
        ('UTF-16 LE text with a BOM', '\ufeffsome %s here\n' % MARK),
        ('UTF-16 LE at an odd offset', b'\0' + MARK.encode('utf-16-le')),
        ('UTF-16 BE text', b'\0\0' + MARK.encode('utf-16-be')),
        ('UTF-16 BE at an odd offset', b'\0\0\0' + MARK.encode('utf-16-be')),
    ]
    for i, (what, data) in enumerate(cases):
        if isinstance(data, str):
            data = data.encode('utf-16-le')
        rc, out = scan(make_repo(tmp, 'bin%d' % i, {'icon.bin': data, 'a.txt': b'hello\n'}))
        check(rc == 1 and 'contents of icon.bin' in out and 'NOT publishable' in out, 'binary blob, %s: a hit' % what)
        check(MARK not in out, 'binary blob, %s: the matched text is not printed' % what)

    # --tree leaves a binary file alone (unchanged behaviour; the scan above is what stops it) and rewrites a text file
    tree = os.path.join(tmp, 'tree'); os.mkdir(tree)
    with open(os.path.join(tree, 't.txt'), 'wb') as f: f.write(b'a ' + m + b'\n')
    with open(os.path.join(tree, 'b.bin'), 'wb') as f: f.write(b'\0' + m)
    p = subprocess.run([sys.executable, SCRUB, '--rules', RULES, '--tree', tree], env=ENV)
    t = open(os.path.join(tree, 't.txt'), 'rb').read(); b = open(os.path.join(tree, 'b.bin'), 'rb').read()
    check(p.returncode == 0 and t == b'a REDACTED\n' and b == b'\0' + m, '--tree rewrites the text file and leaves the binary file as it was')

print('test_scrub: %d checks, %d failed (%s)' % (checks, fails, SCRUB))
sys.exit(1 if fails else 0)
