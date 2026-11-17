#!/usr/bin/env bash
# tools/publish/export.sh : the copy of this repository that may leave the machine.
#
# The working repository keeps what the work needs (home paths, host names of the build machines, session notes).  None of that
# belongs on a hosting service.  This script makes a fresh clone, rewrites EVERY commit with tools/publish/scrub.py and the
# private rules file, scans every commit of the result for what must never be published, and only then (with --push) sends it.
# The rewrite is a pure function of the history and the rules, so an unchanged history exports to the same commits and a later
# export of a longer history is a fast-forward of the earlier one.  Changing the rules changes every commit: avoid it.
#
#   tools/publish/export.sh            build and scan the export in $KHNMC_PUBLISH_DIR (default ~/work/khnmc-publish)
#   tools/publish/export.sh --push     the same, then push branch main to $KHNMC_PUBLISH_URL as $KHNMC_PUBLISH_USER
#
# env: KHNMC_PUBLISH_RULES (default ~/.config/khnmc/publish-rules.tsv; private, never committed)
#      KHNMC_PUBLISH_URL   https URL of the remote (required for --push)
#      KHNMC_PUBLISH_USER  account whose token `gh auth token --user` gives (required for --push); the active gh account is
#                          never switched and no credential helper stores the token
# Exit status: 0 done, 1 the scan found something or a step failed (nothing is pushed), 2 usage.
set -u
HERE=$(cd "$(dirname "$0")/../.." && pwd)
RULES=${KHNMC_PUBLISH_RULES:-$HOME/.config/khnmc/publish-rules.tsv}
OUT=${KHNMC_PUBLISH_DIR:-$HOME/work/khnmc-publish}
PUSH=0; [ "${1:-}" = "--push" ] && PUSH=1
[ $# -le 1 ] && { [ $# -eq 0 ] || [ $PUSH = 1 ]; } || { sed -n 2,18p "$0"; exit 2; }
[ -f "$RULES" ] || { echo "export: no rules file $RULES" >&2; exit 2; }
[ -z "$(git -C "$HERE" status --porcelain)" ] || { echo "export: the working repository has uncommitted changes: commit first" >&2; exit 1; }
SCRUB="$HERE/tools/publish/scrub.py"
rm -rf "$OUT.new" && git clone -q --no-local --single-branch --branch main "$HERE" "$OUT.new" || exit 1
cd "$OUT.new" || exit 1
git remote remove origin
FILTER_BRANCH_SQUELCH_WARNING=1 git filter-branch -f \
    --tree-filter "python3 '$SCRUB' --rules '$RULES' --tree ." \
    --msg-filter "python3 '$SCRUB' --rules '$RULES' --stdin" -- --all > "$OUT.filter.log" 2>&1 || { echo "export: filter-branch failed, see $OUT.filter.log" >&2; exit 1; }
git for-each-ref --format='%(refname)' refs/original | while read -r r; do git update-ref -d "$r"; done
git reflog expire --expire=now --all && git gc -q --prune=now
python3 "$SCRUB" --rules "$RULES" --scan . || { echo "export: NOT publishable; the export is left in $OUT.new for inspection" >&2; exit 1; }
echo "export: $(git rev-list --count main) commits, head $(git rev-parse --short main), authors: $(git log --format='%an <%ae>' | sort -u | tr '\n' ';')"
rm -rf "$OUT" && mv "$OUT.new" "$OUT" && cd "$OUT" || exit 1
[ $PUSH = 1 ] || { echo "export: built in $OUT (not pushed)"; exit 0; }
: "${KHNMC_PUBLISH_URL:?set KHNMC_PUBLISH_URL}" "${KHNMC_PUBLISH_USER:?set KHNMC_PUBLISH_USER}"
gh auth token --user "$KHNMC_PUBLISH_USER" > /dev/null 2>&1 || { echo "export: no gh token for $KHNMC_PUBLISH_USER" >&2; exit 1; }
git -c credential.https://github.com.helper= \
    -c "credential.https://github.com.helper=!f() { test \"\$1\" = get || exit 0; echo username=$KHNMC_PUBLISH_USER; echo \"password=\$(gh auth token --user $KHNMC_PUBLISH_USER)\"; }; f" \
    push "$KHNMC_PUBLISH_URL" main:main || { echo "export: push refused (a non-fast-forward means the rules or the history changed: look before forcing)" >&2; exit 1; }
echo "export: pushed main to $KHNMC_PUBLISH_URL as $KHNMC_PUBLISH_USER"
