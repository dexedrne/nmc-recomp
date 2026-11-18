#!/bin/bash
# tools/ios/push_data.sh [plan|run] : copy the game data (profile A: the data tree minus prune-A.list, plus pkgdir/) to the
# owner's iPhone, into the KHNMC app's Documents/data and Documents/pkgdir, through the MacBook, in chunks.
#   plan : write the chunk lists only (run-J26/ios/push/chunk-NNN.list, about CHUNK_MB each, default 2048)
#   verify: list Documents/data and Documents/pkgdir ON THE PHONE (devicectl info files, JSON) and compare every name and byte size
#          with manifest.tsv: missing, wrong size and extra files are listed (verify.txt); exit 0 only when all match
#   run  : for every chunk without a .done mark: rsync it to ~/Developer/KHNMC/stage/cNNN on the Mac (the NEXT chunk is staged while
#          the current one goes to the phone, so at most two chunks, about 4 GB, sit on the Mac: the project folder stays under its
#          6 GB limit), check file count and bytes there, `devicectl device copy to` into the app's container (devicectl merges
#          into the folder and skips files that did not change), delete the staged chunk, mark it done.  Resumable: run again.
# The phone's id is read from the Mac's ~/Developer/KHNMC/state/device.env.  The app must be installed (it marks data/ and
# pkgdir/ as excluded from iCloud backup at every start; device_run.sh installs it).  Wired or over the network: devicectl
# uses the cable when the phone is plugged into the Mac.
set -u
HERE=$(cd "$(dirname "$0")/../.." && pwd); P=$HERE/run-J26/ios/push; mkdir -p "$P"
MAC=${IOS_MAC:-user@macbook}; RSH="ssh -o BatchMode=yes -o ServerAliveInterval=30"
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
chunks=[]; cur=[]; cs=0; kind=None
for k,r,s in files:
    if cur and (cs+s>lim or k!=kind): chunks.append((kind,cur)); cur=[]; cs=0
    cur.append((r,s)); cs+=s; kind=k
if cur: chunks.append((kind,cur))
for f in os.listdir(out):
    if f.startswith('chunk-') and f.endswith('.list'): os.remove(os.path.join(out,f))
with open(os.path.join(out,'manifest.tsv'),'w') as m:
    for k,r,s in files: m.write('%s\t%s\t%d\n'%(k,r,s))
for i,(k,c) in enumerate(chunks):
    with open(os.path.join(out,'chunk-%03d.list'%i),'w') as f:
        f.write('# %s %d files %d bytes\n'%(k,len(c),sum(s for _,s in c)))
        for r,s in c: f.write(r+'\n')
print('push: plan: %d files, %.2f GB, %d chunks of up to %s MiB (manifest %s/manifest.tsv)'%(len(files),sum(s for _,_,s in files)/1e9,len(chunks),cmb,out))
PY
}
stage(){ local c=$1 n=$(basename "$1" .list) k src; k=$(head -1 "$c" | cut -d' ' -f2); [ "$k" = data ] && src=$DATA || src=$PKG
  grep -v '^#' "$c" > "$P/$n.files"
  $RSH "$MAC" "mkdir -p ~/Developer/KHNMC/stage/$n/$k" || return 1
  rsync -rlpt --files-from="$P/$n.files" -e "$RSH" "$src/" "$MAC:Developer/KHNMC/stage/$n/$k/" >> "$P/rsync.log" 2>&1 || return 1
  local want got; want=$(head -1 "$c" | awk '{print $3" "$5}')
  got=$($RSH "$MAC" "cd ~/Developer/KHNMC/stage/$n/$k && echo \$(find . -type f | wc -l) \$(find . -type f -exec stat -f %z {} + | awk '{s+=\$1} END{print s+0}')") || return 1
  [ "$(echo $got)" = "$want" ] || { log "$n: staged copy differs: want '$want' got '$got'"; return 1; }; }
send(){ local c=$1 n=$(basename "$1" .list) k; k=$(head -1 "$c" | cut -d' ' -f2)
  $RSH "$MAC" "export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer; . ~/Developer/KHNMC/state/device.env; xcrun devicectl device copy to --device \$DEVICE --timeout 7200 --domain-type appDataContainer --domain-identifier beer.vyvanse.khnmc --source ~/Developer/KHNMC/stage/$n/$k --destination Documents/$k" >> "$P/devicectl.log" 2>&1 || return 1
  $RSH "$MAC" "rm -rf ~/Developer/KHNMC/stage/$n"; }
verify(){
  local k; for k in data pkgdir; do
    $RSH "$MAC" "export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer; . ~/Developer/KHNMC/state/device.env; xcrun devicectl device info files --device \$DEVICE --timeout 600 --domain-type appDataContainer --domain-identifier beer.vyvanse.khnmc --subdirectory Documents/$k --json-output ~/Developer/KHNMC/run/phone-$k.json > /dev/null 2>&1; cat ~/Developer/KHNMC/run/phone-$k.json; rm -f ~/Developer/KHNMC/run/phone-$k.json" > "$P/phone-$k.json" || return 1
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
log "run: ${#todo[@]} chunks to send"
[ ${#todo[@]} = 0 ] && exit 0
stage "${todo[0]}" || { log "$(basename "${todo[0]}" .list): staging FAILED (see rsync.log)"; exit 1; }
for ((i=0;i<${#todo[@]};i++)); do
  c=${todo[$i]}; n=$(basename "$c" .list); nx=""; pid=""
  if [ $((i+1)) -lt ${#todo[@]} ]; then nx=${todo[$((i+1))]}; stage "$nx" & pid=$!; fi
  t0=$SECONDS; b=$(head -1 "$c" | awk '{print $5}')
  if send "$c"; then touch "${c%.list}.done"; dt=$((SECONDS-t0)); log "$n: on the phone ($(head -1 "$c" | cut -c3-)) in ${dt} s, $(( b / (dt>0?dt:1) / 1048576 )) MiB/s"
  else log "$n: devicectl copy FAILED (see devicectl.log)"; [ -n "$pid" ] && wait $pid; exit 1; fi
  if [ -n "$pid" ]; then wait $pid || { log "$(basename "$nx" .list): staging FAILED (see rsync.log)"; exit 1; }; fi
done
log "run: every chunk sent"
