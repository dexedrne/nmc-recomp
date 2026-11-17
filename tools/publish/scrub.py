#!/usr/bin/env python3
"""scrub.py : rewrite private strings out of a tree or a commit message, and scan a repository for what must never be published.

  scrub.py --rules RULES --tree DIR      rewrite every text file under DIR in place (git filter-branch --tree-filter)
  scrub.py --rules RULES --stdin         rewrite standard input to standard output (git filter-branch --msg-filter)
  scrub.py --rules RULES --scan REPO     every commit of REPO (all refs): file contents, file names, messages, author and
                                         committer lines are searched for the rules' `forbid` patterns.  Exit 1 and a list of
                                         hits (commit, where, pattern number: never the matched text) if any is found.

RULES is a private tab-separated file that is NOT part of this repository (it has to name what it removes):
  literal <TAB> text    <TAB> replacement
  regex   <TAB> pattern <TAB> replacement
  forbid  <TAB> pattern
Rules are applied in file order.  Lines starting with # are comments.  Binary files (a NUL byte in the first 8 KiB) are left
alone by --tree and are reported by --scan only through their names.
"""
import argparse, os, re, subprocess, sys


def load(path):
    subs, forbid = [], []
    for ln in open(path, encoding='utf-8'):
        ln = ln.rstrip('\n')
        if not ln or ln.startswith('#'):
            continue
        p = ln.split('\t')
        if p[0] == 'literal' and len(p) == 3:
            subs.append((re.compile(re.escape(p[1])), p[2].replace('\\', '\\\\')))
        elif p[0] == 'regex' and len(p) == 3:
            subs.append((re.compile(p[1]), p[2]))
        elif p[0] == 'forbid' and len(p) == 2:
            forbid.append(re.compile(p[1]))
        else:
            sys.exit('scrub: bad rule line: kind %r with %d fields' % (p[0], len(p)))
    return subs, forbid


def rewrite(text, subs):
    for rx, rep in subs:
        text = rx.sub(rep, text)
    return text


def do_tree(root, subs):
    changed = 0
    for d, dirs, files in os.walk(root):
        if '.git' in dirs:
            dirs.remove('.git')
        for fn in files:
            p = os.path.join(d, fn)
            if os.path.islink(p):
                continue
            try:
                b = open(p, 'rb').read()
            except OSError:
                continue
            if b'\0' in b[:8192]:
                continue
            try:
                t = b.decode('utf-8')
            except UnicodeDecodeError:
                t = b.decode('latin-1'); enc = 'latin-1'
            else:
                enc = 'utf-8'
            n = rewrite(t, subs)
            if n != t:
                st = os.stat(p)
                open(p, 'wb').write(n.encode(enc))
                os.chmod(p, st.st_mode)
                changed += 1
    return changed


def do_scan(repo, forbid):
    def git(*a):
        return subprocess.run(['git', '-C', repo] + list(a), check=True, stdout=subprocess.PIPE).stdout
    commits = git('rev-list', '--all').decode().split()
    hits, seen_blobs = [], {}
    for c in commits:
        meta = git('log', '-1', '--format=%an%n%ae%n%cn%n%ce%n%B', c).decode('utf-8', 'replace')
        for i, rx in enumerate(forbid):
            if rx.search(meta):
                hits.append((c[:10], 'commit message or author line', i))
        for ln in git('ls-tree', '-r', '-z', c).split(b'\0'):
            if not ln:
                continue
            head, name = ln.split(b'\t', 1)
            sha = head.split()[2].decode(); name = name.decode('utf-8', 'replace')
            for i, rx in enumerate(forbid):
                if rx.search(name):
                    hits.append((c[:10], 'file name ' + name, i))
            if sha not in seen_blobs:
                b = git('cat-file', 'blob', sha)
                bad = []
                if b'\0' not in b[:8192]:
                    t = b.decode('utf-8', 'replace')
                    bad = [i for i, rx in enumerate(forbid) if rx.search(t)]
                seen_blobs[sha] = bad
            for i in seen_blobs[sha]:
                hits.append((c[:10], 'contents of ' + name, i))
    return len(commits), len(seen_blobs), hits


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--rules', required=True)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument('--tree'); g.add_argument('--stdin', action='store_true'); g.add_argument('--scan')
    a = ap.parse_args()
    subs, forbid = load(a.rules)
    if a.tree:
        do_tree(a.tree, subs)
    elif a.stdin:
        sys.stdout.write(rewrite(sys.stdin.read(), subs))
    else:
        ncommit, nblob, hits = do_scan(a.scan, forbid)
        if hits:
            uniq = sorted(set(hits))
            for c, where, i in uniq[:60]:
                print('scrub: FORBIDDEN pattern #%d in commit %s: %s' % (i + 1, c, where))
            print('scrub: %d hit(s) in %d commits (%d distinct blobs): NOT publishable' % (len(uniq), ncommit, nblob))
            sys.exit(1)
        print('scrub: scan clean: %d commits, %d distinct blobs, %d forbidden patterns, 0 hits' % (ncommit, nblob, len(forbid)))


if __name__ == '__main__':
    main()
