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
  drop    <TAB> path regex (remove matching files and reject matching paths during scan)
  file    <TAB> path regex <TAB> pattern <TAB> replacement (after global substitutions)
  message <TAB> pattern <TAB> replacement (before global substitutions)
  forbid  <TAB> pattern
Rules are applied in file order.  Lines starting with # are comments.  Binary files (a NUL byte in the first 8 KiB) are left
alone by --tree, but --scan searches them too: as UTF-8, as raw bytes (one character per byte, so an ASCII pattern is found
anywhere in the file) and as UTF-16 little and big endian at both byte alignments.  So a binary file that still holds a forbidden
pattern stops the export instead of passing as clean (review 2026-11-17: --scan used to store such a blob as clean without
searching it).  Compressed contents (zip, PNG, PDF streams) are not unpacked: the scan's summary line counts the binary blobs.
"""
import argparse, os, re, shutil, subprocess, sys


def load(path):
    subs, forbid, drop, files, messages = [], [], [], [], []
    for ln in open(path, encoding='utf-8'):
        ln = ln.rstrip('\n')
        if not ln or ln.startswith('#'):
            continue
        p = ln.split('\t')
        if p[0] == 'literal' and len(p) == 3:
            subs.append((re.compile(re.escape(p[1])), p[2].replace('\\', '\\\\')))
        elif p[0] == 'regex' and len(p) == 3:
            subs.append((re.compile(p[1]), p[2]))
        elif p[0] == 'drop' and len(p) == 2:
            drop.append(re.compile(p[1]))
        elif p[0] == 'file' and len(p) == 4:
            files.append((re.compile(p[1]), re.compile(p[2]), p[3]))
        elif p[0] == 'message' and len(p) == 3:
            messages.append((re.compile(p[1]), p[2]))
        elif p[0] == 'forbid' and len(p) == 2:
            forbid.append(re.compile(p[1]))
        else:
            sys.exit('scrub: bad rule line: kind %r with %d fields' % (p[0], len(p)))
    return subs, forbid, drop, files, messages


def rewrite(text, subs):
    for rx, rep in subs:
        text = rx.sub(rep, text)
    return text


def do_tree(root, subs, drop, filesubs):
    changed = 0
    templates = False
    for d, dirs, files in os.walk(root):
        if '.git' in dirs:
            dirs.remove('.git')
        for fn in files:
            p = os.path.join(d, fn)
            name = os.path.relpath(p, root)
            if any(rx.search(name) for rx in drop):
                os.unlink(p)
                continue
            if os.path.islink(p):
                continue
            try:
                b = open(p, 'rb').read()
            except OSError:
                raise
            if b'\0' in b[:8192]:
                continue
            try:
                t = b.decode('utf-8')
            except UnicodeDecodeError:
                t = b.decode('latin-1'); enc = 'latin-1'
            else:
                enc = 'utf-8'
            n = rewrite(t, subs)
            for path_rx, rx, rep in filesubs:
                if path_rx.search(name):
                    n = rx.sub(rep, n)
            if n != t:
                st = os.stat(p)
                open(p, 'wb').write(n.encode(enc))
                os.chmod(p, st.st_mode)
                changed += 1
                if '@KHINPUT(' in n and '@KHINPUT(' not in t:
                    os.rename(p, p + '.kh-input')
                    templates = True
    if templates:
        dest = os.path.join(root, 'tools', 'publish', 'prepare_sources.py')
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        shutil.copyfile(os.path.join(os.path.dirname(__file__), 'prepare_sources.py'), dest)
    return changed


def is_binary(b):
    return b'\0' in b[:8192]


def blob_texts(b):
    """Every form a blob's contents are searched in (see the module's docstring)."""
    texts = [b.decode('utf-8', 'replace')]
    if is_binary(b):
        texts.append(b.decode('latin-1'))
        for enc in ('utf-16-le', 'utf-16-be'):
            texts.append(b.decode(enc, 'replace'))
            texts.append(b[1:].decode(enc, 'replace'))
    return texts


def do_scan(repo, forbid, drop):
    def git(*a):
        return subprocess.run(['git', '-C', repo] + list(a), check=True, stdout=subprocess.PIPE).stdout
    commits = git('rev-list', '--all').decode().split()
    hits, seen_blobs, binary = [], {}, set()
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
            for i, rx in enumerate(forbid + drop):
                if rx.search(name):
                    hits.append((c[:10], 'file name ' + name, i))
            if sha not in seen_blobs:
                b = git('cat-file', 'blob', sha)
                if is_binary(b):
                    binary.add(sha)
                texts = blob_texts(b)
                seen_blobs[sha] = [i for i, rx in enumerate(forbid) if any(rx.search(t) for t in texts)]
            for i in seen_blobs[sha]:
                hits.append((c[:10], 'contents of ' + name + (' (binary)' if sha in binary else ''), i))
    return len(commits), len(seen_blobs), len(binary), hits


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--rules', required=True)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument('--tree'); g.add_argument('--stdin', action='store_true'); g.add_argument('--scan')
    a = ap.parse_args()
    subs, forbid, drop, files, messages = load(a.rules)
    if a.tree:
        do_tree(a.tree, subs, drop, files)
    elif a.stdin:
        sys.stdout.write(rewrite(sys.stdin.read(), messages + subs))
    else:
        ncommit, nblob, nbin, hits = do_scan(a.scan, forbid, drop)
        if hits:
            uniq = sorted(set(hits))
            for c, where, i in uniq[:60]:
                print('scrub: FORBIDDEN pattern #%d in commit %s: %s' % (i + 1, c, where))
            print('scrub: %d hit(s) in %d commits (%d distinct blobs): NOT publishable' % (len(uniq), ncommit, nblob))
            sys.exit(1)
        print('scrub: scan clean: %d commits, %d distinct blobs (%d binary, searched as bytes and UTF-16; compressed contents are '
              'not unpacked), %d forbidden patterns, 0 hits' % (ncommit, nblob, nbin, len(forbid)))


if __name__ == '__main__':
    main()
