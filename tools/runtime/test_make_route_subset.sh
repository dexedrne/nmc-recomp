#!/usr/bin/env bash
# tools/test_make_route_subset.sh : checks of the verification step of tools/make_route_subset.sh (review 2026-11-17, n=10).
#
#   tools/test_make_route_subset.sh [SCRIPT]     SCRIPT = the make_route_subset.sh to test (default: the one next to this file)
#
# Every case runs SCRIPT in check-only mode (-c) in its own new scratch tree (a copy of SCRIPT in <tmp>/tools, dummy binary, data
# and pkgdir, a fake ./run-subset with route_batch's records: rNN/{open.log,files.log,exit} and summary.tsv).  -c runs and builds
# nothing, so no game, no shim, no route_batch.  The scratch trees are removed at the end.  Exit 0 when every case passed.
set -u
SELF=$(cd "$(dirname "$0")" && pwd)
SCRIPT=$(realpath "${1:-$SELF/make_route_subset.sh}")
[ -f "$SCRIPT" ] || { echo "test_make_route_subset: no script $SCRIPT" >&2; exit 2; }
TMPR=$(mktemp -d "${TMPDIR:-/tmp}/test-mrs.XXXXXX") || exit 2
trap 'rm -rf "$TMPR"' EXIT
pass=0; fail=0; n=0

# a fresh tree: $H/{khnmc,data-full,pkgdir,tools/*,route-subset/{data,pkgdir,MANIFEST.tsv}}
newtree(){ n=$((n+1)); H="$TMPR/t$n"; mkdir -p "$H/tools" "$H/data-full" "$H/pkgdir" "$H/route-subset/data" "$H/route-subset/pkgdir" "$H/run-subset"
  cp "$SCRIPT" "$H/tools/make_route_subset.sh"; : > "$H/khnmc"; : > "$H/tools/openlog_shim.c"; : > "$H/tools/route_subset.py"; : > "$H/tools/route_batch.sh"
  printf 'path\tkind\tsize\tbytes_kept\tsha256_or_ranges\ndata/a.bin\tname-only\t0\t0\t-\n' > "$H/route-subset/MANIFEST.tsv"; }
# a run set as route_batch leaves it: $1 = set directory, $2 = which tree (full|subset), $3 = exit record (- = none),
# $4 = summary result (- = no summary.tsv), $5 = files.log contents (- = no files.log)
mkrun(){ local s=$1 tree=$2 ex=$3 res=$4 fl=$5 r="$1/r01" root
  mkdir -p "$r"; if [ "$tree" = full ]; then root="$H/data-full"; else root="$H/route-subset/data"; fi
  printf 'open ok %s/a.bin\nstat ok %s\n' "$root" "$root" > "$r/open.log"
  [ "$ex" = - ] || echo "$ex" > "$r/exit"
  [ "$fl" = - ] || printf '%s\n' "$fl" > "$r/files.log"
  [ "$res" = - ] || printf 'run\texit\tresult\tpresents\nr01\t%s\t%s\t100\n' "${ex/-/missing}" "$res" > "$s/summary.tsv"; }
# $1 = what, $2 = expected exit status, $3 = text the output must contain, then the arguments
expect(){ local what=$1 want=$2 text=$3; shift 3; local out rc
  out=$(cd "$H" && bash "$H/tools/make_route_subset.sh" -c -o "$H/route-subset" "$@" 2>&1); rc=$?
  if [ "$rc" = "$want" ] && grep -qF -- "$text" <<<"$out"; then pass=$((pass+1)); echo "ok    $what (exit $rc)"
  else fail=$((fail+1)); echo "FAIL  $what: exit $rc (want $want), output must contain '$text'; output:"; sed 's/^/        /' <<<"$out" | tail -8; fi; }
A="HIT LOOSE 1 a.bin"; B="HIT LOOSE 1 a.bin
MISS LOOSE 0 b.bin"

newtree; for d in 0 1; do mkrun "$H/run-subset/full-$d" full 0 ok "$A"; mkrun "$H/run-subset/subset-$d" subset 0 ok "$A"; done
expect "all runs ok, logs equal: passes" 0 "verification PASSED" -d "0 1"

for c in "139 FAIL" "124 FAIL" "0 DIVERGED"; do set -- $c
  newtree; mkrun "$H/run-subset/full-0" full 0 ok "$A"; mkrun "$H/run-subset/subset-0" subset "$1" "$2" "$A"
  expect "subset run with exit $1 / $2 and the same logs: fails" 1 "the subset run did not pass" -d 0
done
newtree; mkrun "$H/run-subset/full-0" full 0 ok "$A"; mkrun "$H/run-subset/subset-0" subset - - "$A"
expect "subset run without exit record or summary: fails" 1 "the subset run did not pass: no exit record" -d 0

newtree; mkrun "$H/run-subset/full-0" full 139 FAIL "$A"; mkrun "$H/run-subset/subset-0" subset 0 ok "$A"
expect "reference run with exit 139: fails" 1 "VERIFY reference run full-0" -d 0

newtree; mkrun "$H/run-subset/full-0" full 0 ok "$A"; mkrun "$H/run-subset/full-9" full 0 ok "$B"; mkrun "$H/run-subset/subset-0" subset 0 ok "$B"
expect "subset log equal only to an unselected full-* run: fails" 1 "equals NO full-tree run" -d 0

newtree; mkdir -p "$H/elsewhere"; mkrun "$H/elsewhere/story-1" full 0 ok -; mkrun "$H/run-subset/subset-0" subset 0 ok "$A"
expect "-L reference without a game file log: fails" 1 "no reference run has a game file log" -d 0 -L "$H/elsewhere/story-1/r01/open.log"

newtree; mkdir -p "$H/elsewhere"; mkrun "$H/elsewhere/story-1" full 0 ok "$A"; mkrun "$H/run-subset/subset-0" subset 0 ok "$A"
expect "-L reference with an equal game file log: passes" 0 "identical to the full-tree run(s) story-1" -d 0 -L "$H/elsewhere/story-1/r01/open.log"

newtree; mkrun "$H/run-subset/full-0" full 0 ok "$A"; mkrun "$H/run-subset/subset-0" subset 0 ok "$A"
expect "empty -d: usage error" 2 "no difficulty setting" -d " "
expect "-L naming a missing open log: usage error" 2 "no open log" -d 0 -L "$H/nowhere/open.log"

echo "test_make_route_subset: $((pass+fail)) cases, $fail failed ($SCRIPT)"
[ "$fail" = 0 ]
