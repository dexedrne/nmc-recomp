#!/bin/bash
# tools/ios/push_data.sh [plan|run] : copy the game data (profile A: the data tree minus prune-A.list, plus pkgdir/) to the
# owner's iPhone, into the KHNMC app's Documents/data and Documents/pkgdir, through the MacBook, in chunks.
#   plan : write the chunk lists only (run-J26/ios/push/chunk-NNN.list, about CHUNK_MB each, default 2048)
#   verify: list Documents/data and Documents/pkgdir ON THE PHONE (devicectl info files, JSON) and compare every name and byte size
#          with manifest.tsv: missing, wrong size and extra files are listed (verify.txt); exit 0 only when all match
#   run  : for every chunk without a .done mark: rsync it to ~/Developer/KHNMC/stage/cNNN on the Mac (KHNMC_IOS_RDIR changes the root) (the NEXT chunk is staged while
#          the current one goes to the phone, so at most two chunks, about 4 GB, sit on the Mac: the project folder stays under its
#          6 GB limit), check file count and bytes there, `devicectl device copy to` into the app's container (devicectl merges
#          into the folder and skips files that did not change), delete the staged chunk, mark it done.  Resumable: run again.
# The phone's id is read from the Mac's ~/Developer/KHNMC/state/device.env (under KHNMC_IOS_RDIR).  The app must be installed (it marks data/ and
# pkgdir/ as excluded from iCloud backup at every start; device_run.sh installs it).  Wired or over the network: devicectl
# uses the cable when the phone is plugged into the Mac.
# Review fixes: common.sh is sourced (KHNMC_IOS_MACBOOK_OK guard, the Linux driver lock, KHNMC_IOS_MAC / KHNMC_IOS_RDIR honoured);
# pipelines are judged by the status of the step before `tee` (pipefail), not by tee's; a chunk that fails to send is removed from
# the Mac (trap), so a re-plan starts clean.
set -u -o pipefail
. "$(dirname "$0")/common.sh"
case "${1:-run}" in plan) ;; *) ios_guard;; esac     # plan only reads this machine; verify and run talk to the Mac
HERE=$IOS_HERE; P=$HERE/run-J26/ios/push; mkdir -p "$P"
MAC=$IOS_MAC; RDIR=$IOS_RDIR; RSH=$IOS_RSH" -o ServerAliveInterval=30"
DATA=$HERE/data-full; PKG=$HERE/pkgdir; PRUNE=$HERE/prune-A.list; CHUNK_MB=${CHUNK_MB:-2048}
log(){ echo "push: $(date +%H:%M:%S) $*" | tee -a "$P/push.log"; }
plan(){
  python3 - "$DATA" "$PKG" "$PRUNE" "$P" "$CHUNK_MB" <<'PY'
import os,sys
data,pkg,prune,out,cmb=sys.argv[1:6]; lim=int(cmb)*1048576
A=set(l.rstrip('\n') for l in open(prune) if l.strip() and not l.startswith('#'))
files=[]
for root,ds,fs in os.walk(data):
    ds.sort()
    for f in sorted(fs):
        p=os.path.join(root,f); r=os.path.relpath(p,data)
        if r in A: continue
        files.append(('data',r,os.path.getsize(p)))
for root,ds,fs in os.walk(pkg):
    for f in sorted(fs):
        p=os.path.join(root,f); files.append(('pkgdir',os.path.relpath(p,pkg),os.path.getsize(p)))
big=[(k,r,s) for k,r,s in files if s>lim]
if big: sys.exit('push: plan: %d file(s) are larger than a chunk (%s MiB): %s -- raise CHUNK_MB only if two of them fit the Mac folder\'s limit'%(len(big),cmb,', '.join('%s/%s %d MiB'%(k,r,s>>20) for k,r,s in big[:5])))
chunks=[]; cur=[]; cs=0; kind=None
for k,r,s in files:
    if cur and (cs+s>lim or k!=kind): chunks.append((kind,cur)); cur=[]; cs=0
    cur.append((r,s)); cs+=s; kind=k
if cur: chunks.append((kind,cur))
for f in os.listdir(out):   # old lists AND their done marks (review r9: a new chunk must not inherit an old number's mark)
    if f.startswith('chunk-') and (f.endswith('.list') or f.endswith('.done') or f.endswith('.files')): os.remove(os.path.join(out,f))
with open(os.path.join(out,'manifest.tsv'),'w') as m:
    for k,r,s in files: m.write('%s\t%s\t%d\n'%(k,r,s))
for i,(k,c) in enumerate(chunks):
    with open(os.path.join(out,'chunk-%03d.list'%i),'w') as f:
        f.write('# %s %d files %d bytes\n'%(k,len(c),sum(s for _,s in c)))
        for r,s in c: f.write(r+'\n')
print('push: plan: %d files, %.2f GB, %d chunks of up to %s MiB (manifest %s/manifest.tsv)'%(len(files),sum(s for _,_,s in files)/1e9,len(chunks),cmb,out))
PY
}
STAGED=()   # the chunks staged on the Mac by this run (recorded by the parent before a background stage starts)
unstage(){ $RSH "$MAC" "rm -rf ~/$RDIR/stage/$1"; }
# review fix #8: whatever this run staged and did not finish is removed from the Mac when the run ends or is interrupted
cleanup_stage(){ local n j; trap - EXIT; for j in $(jobs -p); do kill "$j" 2>/dev/null; done; wait 2>/dev/null   # our own background staging first
  for n in ${STAGED[@]+"${STAGED[@]}"}; do
    [ -f "$P/$n.done" ] || unstage "$n" > /dev/null 2>&1 || echo "push: could not remove the staged $n on the Mac (~/$RDIR/stage/$n): remove it by hand" >&2; done; }
trap cleanup_stage EXIT; trap 'exit 130' INT TERM HUP
stage(){ local c=$1 n=$(basename "$1" .list) k src; k=$(head -1 "$c" | cut -d' ' -f2); [ "$k" = data ] && src=$DATA || src=$PKG
  grep -v '^#' "$c" > "$P/$n.files"
  unstage "$n" || return 1     # a copy left by an earlier or re-planned run must not add files to this chunk
  $RSH "$MAC" "mkdir -p ~/$RDIR/stage/$n/$k" || return 1
  rsync -rlpt --files-from="$P/$n.files" -e "$RSH" "$src/" "$MAC:$RDIR/stage/$n/$k/" >> "$P/rsync.log" 2>&1 || return 1
  local want got; want=$(head -1 "$c" | awk '{print $3" "$5}')
  got=$($RSH "$MAC" "cd ~/$RDIR/stage/$n/$k && echo \$(find . -type f | wc -l) \$(find . -type f -exec stat -f %z {} + | awk '{s+=\$1} END{print s+0}')") || return 1
  [ "$(echo $got)" = "$want" ] || { log "$n: staged copy differs: want '$want' got '$got'"; return 1; }; }
send(){ local c=$1 n=$(basename "$1" .list) k; k=$(head -1 "$c" | cut -d' ' -f2)
  $RSH "$MAC" "export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer; . ~/$RDIR/state/device.env; xcrun devicectl device copy to --device \$DEVICE --timeout 7200 --domain-type appDataContainer --domain-identifier beer.vyvanse.khnmc --source ~/$RDIR/stage/$n/$k --destination Documents/$k" >> "$P/devicectl.log" 2>&1 || return 1
  unstage "$n"; }
verify(){
  local k; for k in data pkgdir; do
    $RSH "$MAC" "export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer; . ~/$RDIR/state/device.env; xcrun devicectl device info files --device \$DEVICE --timeout 600 --domain-type appDataContainer --domain-identifier beer.vyvanse.khnmc --subdirectory Documents/$k --json-output ~/$RDIR/run/phone-$k.json > /dev/null 2>&1; cat ~/$RDIR/run/phone-$k.json; rm -f ~/$RDIR/run/phone-$k.json" > "$P/phone-$k.json" || return 1
  done
  python3 - "$P" <<'PY'
import json,os,sys
P=sys.argv[1]; want={}
for l in open(os.path.join(P,'manifest.tsv')):
    k,r,s=l.rstrip('\n').split('\t'); want[(k,r)]=int(s)
have={}
for k in ('data','pkgdir'):
    for f in json.load(open(os.path.join(P,'phone-%s.json'%k)))['result']['files']:
        if not f['resources']['isDirectory']: have[(k,f['relativePath'])]=f['metadata']['size']
miss=[x for x in want if x not in have]; bad=[x for x in want if x in have and have[x]!=want[x]]; extra=[x for x in have if x not in want]
with open(os.path.join(P,'verify.txt'),'w') as o:
    for tag,lst in (('MISSING',miss),('SIZE',bad),('EXTRA',extra)):
        for k,r in sorted(lst): o.write('%s\t%s/%s\n'%(tag,k,r))
if extra: print('push: verify: WARNING: %d file(s) on the phone are not in the manifest (left from an earlier push; devicectl cannot delete them; the game does not read them): %s/verify.txt'%(len(extra),P))
print('push: verify: %d of %d files on the phone with the right size (%.2f GB); missing %d, wrong size %d, extra %d (list: %s/verify.txt)'%(
    len(want)-len(miss)-len(bad),len(want),sum(want[x] for x in want if x in have and have[x]==want[x])/1e9,len(miss),len(bad),len(extra),P))
sys.exit(0 if not miss and not bad else 1)
PY
}
case "${1:-run}" in
  plan) plan; exit;;
  verify) verify; exit;;
  run) ;;
  *) sed -n '2,14p' "$0" >&2; exit 2;;
esac
ls "$P"/chunk-*.list >/dev/null 2>&1 || plan | tee -a "$P/push.log"
todo=(); for c in "$P"/chunk-*.list; do [ -f "${c%.list}.done" ] || todo+=("$c"); done
if [ ${#todo[@]} = 0 ]; then
  # review r9: local done marks alone said "nothing to do" after the phone's container had been wiped; the phone decides
  log "run: every chunk is marked done here: checking the phone"
  if verify | tee -a "$P/push.log"; then exit 0; fi
  python3 - "$P" <<'PY'
import glob,os,sys
P=sys.argv[1]; bad=set()
for l in open(os.path.join(P,'verify.txt')):
    tag,path=l.rstrip('\n').split('\t',1)
    if tag in ('MISSING','SIZE'): bad.add(path)
for c in sorted(glob.glob(os.path.join(P,'chunk-*.list'))):
    lines=open(c).read().split('\n'); kind=lines[0].split()[1]
    if any(kind+'/'+r in bad for r in lines[1:] if r):
        d=c[:-5]+'.done'
        if os.path.exists(d): os.remove(d); print('push: %s will be sent again'%os.path.basename(c[:-5]))
PY
  todo=(); for c in "$P"/chunk-*.list; do [ -f "${c%.list}.done" ] || todo+=("$c"); done
fi
log "run: ${#todo[@]} chunks to send"
[ ${#todo[@]} = 0 ] && exit 0
STAGED+=("$(basename "${todo[0]}" .list)")
stage "${todo[0]}" || { log "$(basename "${todo[0]}" .list): staging FAILED (see rsync.log)"; exit 1; }
for ((i=0;i<${#todo[@]};i++)); do
  c=${todo[$i]}; n=$(basename "$c" .list); nx=""; pid=""
  if [ $((i+1)) -lt ${#todo[@]} ]; then nx=${todo[$((i+1))]}; STAGED+=("$(basename "$nx" .list)"); stage "$nx" & pid=$!; fi
  t0=$SECONDS; b=$(head -1 "$c" | awk '{print $5}')
  if send "$c"; then touch "${c%.list}.done"; dt=$((SECONDS-t0)); log "$n: on the phone ($(head -1 "$c" | cut -c3-)) in ${dt} s, $(( b / (dt>0?dt:1) / 1048576 )) MiB/s"
  else log "$n: devicectl copy FAILED (see devicectl.log)"; [ -n "$pid" ] && wait $pid; exit 1; fi
  if [ -n "$pid" ]; then wait $pid || { log "$(basename "$nx" .list): staging FAILED (see rsync.log)"; exit 1; }; fi
done
log "run: every chunk sent; checking the phone"
verify | tee -a "$P/push.log"; exit "${PIPESTATUS[0]}"
