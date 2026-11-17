#!/usr/bin/env bash
# mod_update_check_proof.sh - run tools/mod_update_check.py on OLDER versions of the mod and compare what it finds with git (task 34).
#
#   tools/mod_update_check_proof.sh [-c CLONE] [-o OUTDIR] [-w WORKDIR] COMMIT [COMMIT ...]
#
# For each COMMIT of the mod's own history: `git archive COMMIT` of the clone (read only: no checkout, no branch, no worktree) is
# unpacked into WORKDIR/<commit> (default ./t34-mods, on disk: one version at a time, deleted afterwards), the checker runs on it
# with --json, and its file sets are compared with `git diff --name-status --no-renames COMMIT <pinned>`:
#   git D (in COMMIT, not pinned) = checker "added";  git A (pinned only) = checker "removed";  git M = checker "changed"
# The pinned checkout's working tree has CR LF in its 14 ini files where the commits have LF (`git diff --ignore-cr-at-eol` is empty
# there), so a file git does not list can be "line endings only" for the checker: those must be exactly the CR LF files that are
# unchanged between COMMIT and the pin.  Output: OUTDIR/<commit>.txt (the checker's report), <commit>.json, <commit>.git-diff,
# and one summary line per commit.  Exit status 0 when every set matches, 1 otherwise.
set -u
CLONE=/home/user/projects/SillyBillyMod1
OUT=run-T34-proof
WORK=t34-mods
while getopts "c:o:w:" o; do case "$o" in c) CLONE=$OPTARG;; o) OUT=$OPTARG;; w) WORK=$OPTARG;; *) exit 2;; esac; done
shift $((OPTIND - 1))
[ $# -ge 1 ] || { echo "usage: $0 [-c CLONE] [-o OUTDIR] [-w WORKDIR] COMMIT ..." >&2; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
PIN=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['mod']['commit'])" "$HERE/../mod/nmc-version.json")
mkdir -p "$OUT" "$WORK"
G="git --no-optional-locks -C $CLONE"
bad=0
for c in "$@"; do
    full=$($G rev-parse --verify "$c^{commit}") || { echo "proof: no commit $c" >&2; bad=1; continue; }
    d="$WORK/$c"
    [ -e "$d" ] && { echo "proof: $d exists: remove it first" >&2; bad=1; continue; }
    mkdir -p "$d"
    $G archive --format=tar "$full" | nice -n 10 tar -x -C "$d" || { echo "proof: git archive of $c failed" >&2; bad=1; rm -rf "$d"; continue; }
    $G diff --name-status --no-renames "$full" "$PIN" > "$OUT/$c.git-diff"
    $G diff --stat=200 "$full" "$PIN" | tail -1 > "$OUT/$c.git-stat"
    nice -n 10 python3 "$HERE/mod_update_check.py" --all-files --json "$OUT/$c.json" "$d" > "$OUT/$c.txt" 2> "$OUT/$c.err"
    rc=$?
    python3 - "$OUT/$c.json" "$OUT/$c.git-diff" "$CLONE" "$c" "$rc" "$OUT/$c.git-stat" "$PIN" <<'EOF' || bad=1
import json, sys, subprocess
js, gd, clone, c, rc, gs, pin = sys.argv[1:8]
J = json.load(open(js))
tool = {'added': set(), 'removed': set(), 'changed': set(), 'line endings only': set()}
for k, d in J.get('by_kind', {}).items():
    for w, v in d.items():
        tool[w] |= set(v)
case = [t for s, t in J.get('lines', []) if s == 'files' and t.startswith('case of the name changed')]
git = {'D': set(), 'A': set(), 'M': set()}
for ln in open(gd):
    st, p = ln.rstrip('\n').split('\t', 1)
    git.setdefault(st[0], set()).add(p)
crlf = set(subprocess.run(['git', '--no-optional-locks', '-C', clone, 'diff', '--name-only'], capture_output=True, text=True).stdout.split())
ok_add = tool['added'] == git['D']
ok_rm = tool['removed'] == git['A']
ok_ch = tool['changed'] == git['M']
ok_eol = tool['line endings only'] == crlf - git['M'] - git['A'] - git['D']
ok = ok_add and ok_rm and ok_ch and ok_eol and not case and rc == '1'
print('%s  checker exit %s | added %d (git D %d) %s | removed %d (git A %d) %s | changed %d (git M %d) %s | line endings only %d '
      '(CR LF ini files unchanged since %s: %d) %s | steps %s | loud %d | git --stat: %s' % (
      c, rc, len(tool['added']), len(git['D']), 'ok' if ok_add else 'DIFF', len(tool['removed']), len(git['A']), 'ok' if ok_rm else 'DIFF',
      len(tool['changed']), len(git['M']), 'ok' if ok_ch else 'DIFF', len(tool['line endings only']), c,
      len(crlf - git['M'] - git['A'] - git['D']), 'ok' if ok_eol else 'DIFF', ','.join(J.get('steps', {})), len(J.get('loud', [])),
      open(gs).read().strip()))
for name, a, b in (('added-vs-D', tool['added'], git['D']), ('removed-vs-A', tool['removed'], git['A']), ('changed-vs-M', tool['changed'], git['M'])):
    if a != b:
        print('   %s: only checker %s; only git %s' % (name, sorted(a - b)[:8], sorted(b - a)[:8]))
if case:
    print('   case renames reported: %s' % case[:4])
sys.exit(0 if ok else 1)
EOF
    rm -rf "$d"
done
rmdir "$WORK" 2>/dev/null
exit $bad
