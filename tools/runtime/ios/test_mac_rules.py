#!/usr/bin/env python3
"""tools/ios/test_mac_rules.py [IOS_DIR] : checks, by READING them, that the UNRUN MacBook Air scripts encode that Mac's rules
(TASK26-original-macbook.md).  Nothing in tools/ios/ is executed; IOS_DIR defaults to the directory of this file.

  1. memory gate: `mem_gate` ends the script with `exit 3`, which inside a pipeline (`mem_gate X | tee`) ends only the
     pipeline's subshell.  Every piped call must check `${PIPESTATUS[0]}` and exit on the same line.
  2. mac/build.sh takes a fresh memory reading before EVERY heavy step (each `make -j$JOBS ...` and `xcodebuild`): a mem_gate
     call between the previous heavy step and this one.
  3. mac/sim_run.sh mutes the Mac before any simulator is created or booted, stops when muting fails, and reads the mute state
     back (`output muted of (get volume settings)` must be `true`) before `simctl create` / `simctl boot`.
  4. every xcodebuild call keeps Xcode's derived data and module cache inside ~/Developer/KHNMC (`-derivedDataPath "$KROOT/...`
     and `MODULE_CACHE_DIR="$KROOT/...`); mac/cleanup.sh removes that derived-data folder with the other intermediates.
  5. every `simctl list` names at most ONE category (devices | devicetypes | runtimes | pairs): its syntax is
     `list [-j] [-v] [CATEGORY] [SEARCH TERM | available]`, so a second category word is only a search term, and the JSON then
     holds the first category's section alone.  A `-j` call piped into a parser must request every section the parser reads
     (no category = all of them).
  6. sync.sh copies the sources of every work tree into the ONE fixed ~/Developer/KHNMC/src.  Those copies must not keep this
     machine's modification times (rsync -a / -t): a changed file could arrive with a time OLDER than the object the Mac compiled
     from the previous copy, and mac/build.sh's make would keep the stale object.  They compare by content (-c) instead, so
     exactly the changed files get a fresh time there; every source copy goes through that one rsync definition.
  7. the GFX switch (TASK38): mac/lib.sh accepts GFX=0|1 only and defines gfx_lib_check and gfx_ldflags; every xcodebuild call of
     mac/build.sh and mac/sign_app.sh passes the GFX link flags (${GX[@]+"${GX[@]}"}, filled only when GFX=1: a graphics library
     links only with them) and comes after a gfx_lib_check call in that script; mac/device_run.sh writes KHNMC_GFX=vkd3dw, VKD3D_DEBUG=warn
     and VKD3D_SHADER_DEBUG=warn only inside `if [ "$GFX" = 1 ]` (TASK43 and its review: warn in the test configurations; in the play one
     warn ONLY together with the runtime's log filter KHNMC_GFX_VKD3D_LOG_REPEATS and only when the signed app knows it, else fixme; `logcap`
     only when the signed app knows it; the PIPECACHE lines only for their values; KHNMC_GFX_PIPECACHE_RESET never in the play
     configuration, which stays on the phone: PIPECACHE=fresh is refused outside route mode and gfx_play_lines takes its cache line from
     pc_play_lines, which turns the cache off until $STATE/pipecache-verified exists); the app project carries KHNMC_GFX_LDFLAGS empty and appends it to its
     OTHER_LDFLAGS (both configurations); common.sh hands GFX=1 to the Mac.  Review X38 n=3: lib.sh defines gfx_has, and no mac/*.sh
     pipes nm into grep (an unreadable file would read as "no backend").
Exit 0 when every check holds, 1 otherwise (each failure named)."""
import os, re, shlex, sys

D = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__)))
fails, n = [], 0


def check(ok, what):
    global n
    n += 1
    if not ok:
        fails.append(what)


def lines(rel):
    """the script's logical lines (backslash continuations joined), comments and blank lines dropped: (first line no, text)"""
    out, buf, start = [], '', None
    for i, raw in enumerate(open(os.path.join(D, rel)).read().split('\n'), 1):
        if start is None:
            if raw.lstrip().startswith('#') or not raw.strip():
                continue
            start = i
        if raw.endswith('\\'):
            buf += raw[:-1] + ' '
            continue
        out.append((start, buf + raw))
        buf, start = '', None
    return out


macs = sorted(f for f in os.listdir(os.path.join(D, 'mac')) if f.endswith('.sh'))
check(bool(macs), 'no tools/ios/mac/*.sh found in %s' % D)

# 1. piped memory gates
for f in macs:
    for no, t in lines('mac/' + f):
        if re.search(r'(^|[;&|{(]\s*)mem_gate\s+"', t) and re.search(r'mem_gate\s+"[^"]*"[^;]*\|', t):
            check('PIPESTATUS[0]' in t and re.search(r'\|\|\s*\{?.*?\bexit\s+[1-9]', t) is not None,
                  'mac/%s:%d: mem_gate is piped and its exit status is not checked (exit 3 ends only the subshell): %s'
                  % (f, no, t.strip()[:120]))

# 2. a gate before every heavy step of build.sh
heavy_seen, gate_since = 0, False
for no, t in lines('mac/build.sh'):
    if re.search(r'(^|[;&|{(]\s*)mem_gate\s+"', t):
        gate_since = True
    if re.search(r'-j\$JOBS|\bxcodebuild\b', t) and not t.lstrip().startswith('mem_gate'):
        heavy_seen += 1
        check(gate_since, 'mac/build.sh:%d: heavy step without a fresh memory reading before it: %s' % (no, t.strip()[:100]))
        gate_since = False
check(heavy_seen >= 3, 'mac/build.sh: expected at least 3 heavy steps (shards, applib, xcodebuild), found %d' % heavy_seen)

# 3. mute first, fail closed, read back
sim = lines('mac/sim_run.sh')
idx = lambda pat: next((k for k, (no, t) in enumerate(sim) if re.search(pat, t)), None)
k_mute, k_read = idx(r"set volume with output muted"), idx(r"output muted of \(get volume settings\)")
k_create, k_boot = idx(r"simctl create\b"), idx(r"simctl boot\b")
check(None not in (k_mute, k_create, k_boot), 'mac/sim_run.sh: mute, simctl create or simctl boot not found')
if None not in (k_mute, k_create, k_boot):
    check(k_mute < k_create < k_boot, 'mac/sim_run.sh: the mute does not come before simctl create / boot')
    t = sim[k_mute][1]
    check(re.search(r"\|\|\s*\{.*?\bexit\s+[1-9]", t) is not None,
          'mac/sim_run.sh:%d: a failed mute does not stop the script: %s' % (sim[k_mute][0], t.strip()[:100]))
    check(k_read is not None and k_mute <= k_read < k_create,
          'mac/sim_run.sh: the mute state is not read back between the mute and simctl create')
    if k_read is not None and k_read < k_create:
        between = ' '.join(t for _, t in sim[k_read:k_create])
        check(re.search(r'=\s*"?true"?\s*\]\s*\|\|\s*\{.*?\bexit\s+[1-9]', between) is not None,
              'mac/sim_run.sh: the read-back mute state is not required to be "true" (exit otherwise) before simctl create')

# 4. xcodebuild stays inside ~/Developer/KHNMC
xc = 0
for f in sorted(os.listdir(D)) + ['mac/' + m for m in macs]:
    if not f.endswith('.sh'):
        continue
    for no, t in lines(f):
        if re.search(r'(^|[;&|{(]\s*|nice -n \S+\s+)xcodebuild\s', t):
            xc += 1
            check(re.search(r'-derivedDataPath\s+"\$KROOT/', t) is not None,
                  '%s:%d: xcodebuild without -derivedDataPath inside $KROOT (Xcode writes ~/Library/Developer/Xcode/DerivedData)' % (f, no))
            check(re.search(r'MODULE_CACHE_DIR="\$KROOT/', t) is not None,
                  '%s:%d: xcodebuild without MODULE_CACHE_DIR inside $KROOT (the shared ModuleCache.noindex)' % (f, no))
check(xc >= 1, 'no xcodebuild call found (expected one in mac/build.sh)')
dd = re.search(r'-derivedDataPath\s+"\$KROOT/([^"]+)"', ' '.join(t for _, t in lines('mac/build.sh')))
if dd:
    cl = ' '.join(t for _, t in lines('mac/cleanup.sh'))
    check(re.search(r'objects\)[^;]*rm -rf[^;]*"\$KROOT/%s"' % re.escape(dd.group(1)), cl) is not None,
          'mac/cleanup.sh: `objects` does not remove the derived-data folder $KROOT/%s' % dd.group(1))

# 5. simctl list: one category; a JSON parser reads only sections that were asked for
CATS = ('devices', 'devicetypes', 'runtimes', 'pairs')
sl = 0
for f in macs:
    ls = lines('mac/' + f)
    for k, (no, t) in enumerate(ls):
        m = re.search(r'\bxcrun\s+simctl\s+list\b([^|;)]*)', t)
        if not m:
            continue
        sl += 1
        words = m.group(1).split()
        cats = [w for w in words if not w.startswith('-') and w in CATS]
        check(len(cats) <= 1, 'mac/%s:%d: `simctl list` names %d categories (%s): the second is only a search term'
              % (f, no, len(cats), ' '.join(cats)))
        if '-j' in words or '--json' in words:
            py = re.search(r"python3\s+-c\s+'([^']*)'", '\n'.join(x for _, x in ls[k:k + 40]))
            check(py is not None, 'mac/%s:%d: `simctl list -j` is not read by a python3 -c parser this check knows' % (f, no))
            if py and cats:
                read = set(re.findall(r'(?:\[|\.get\()\s*"(%s)"' % '|'.join(CATS), py.group(1)))
                check(read <= {cats[0]}, 'mac/%s:%d: the parser reads %s but `simctl list -j %s` returns only "%s"'
                      % (f, no, '/'.join(sorted(read - {cats[0]})), cats[0], cats[0]))
check(sl >= 2, 'expected `simctl list` calls in mac/*.sh (sim_run.sh, cleanup.sh), found %d' % sl)

# 6. sync.sh: source copies without the local modification times, compared by content
sy = lines('sync.sh')
rs = [t for _, t in sy if re.match(r'\s*RS=\(', t)]
check(len(rs) == 1, 'sync.sh: expected one RS=( rsync ... ) definition for the source copies, found %d' % len(rs))
if len(rs) == 1:
    toks = shlex.split(rs[0][rs[0].index('(') + 1:rs[0].rindex(')')])
    short = ''.join(x[1:] for x in toks if x.startswith('-') and not x.startswith('--'))
    longs = [x for x in toks if x.startswith('--')]
    check(toks[:1] == ['rsync'], 'sync.sh: RS is not an rsync command: %s' % rs[0].strip()[:100])
    check('a' not in short and 't' not in short and not {'--archive', '--times'} & set(longs),
          'sync.sh: the source copies keep this machine\'s modification times (rsync -a / -t): make on the Mac can keep stale objects')
    check('c' in short or '--checksum' in longs, 'sync.sh: the source copies do not compare by content (rsync -c)')
src_copies = 0
for no, t in sy:
    if re.search(r'\$IOS_MAC:\$IOS_RDIR/src/', t) and re.search(r'\brsync\b|\$\{RS\[@\]\}', t):
        src_copies += 1
        check(t.lstrip().startswith('"${RS[@]}"'), 'sync.sh:%d: a source copy that does not use "${RS[@]}": %s' % (no, t.strip()[:100]))
check(src_copies >= 7, 'sync.sh: expected at least 7 source copies into $IOS_RDIR/src, found %d' % src_copies)

# 7. the GFX switch (TASK38)
lib = ' '.join(t for _, t in lines('mac/lib.sh'))
check(re.search(r'GFX=\$\{GFX:-0\}; case "\$GFX" in 0\|1\) ;; \*\).*?exit 2', lib) is not None, 'mac/lib.sh: GFX is not limited to 0 or 1 (exit 2 otherwise)')
check(re.search(r'\bgfx_lib_check\(\)', lib) is not None and re.search(r'\bgfx_ldflags\(\)', lib) is not None, 'mac/lib.sh: gfx_lib_check or gfx_ldflags is missing')
for f in ('mac/build.sh', 'mac/sign_app.sh'):
    ls = lines(f); kcheck = next((k for k, (no, t) in enumerate(ls) if re.search(r'(^|[;&|{(]\s*|\s)gfx_lib_check\s', t)), None)
    xcs = [(k, no, t) for k, (no, t) in enumerate(ls) if re.search(r'(^|[;&|{(]\s*|nice -n \S+\s+)xcodebuild\s', t)]
    check(bool(xcs), '%s: no xcodebuild call' % f)
    for k, no, t in xcs:
        check('${GX[@]+"${GX[@]}"}' in t, '%s:%d: xcodebuild without the GFX link flags ${GX[@]+"${GX[@]}"}: a GFX=1 library would not link' % (f, no))
        check(kcheck is not None and kcheck < k, '%s:%d: xcodebuild without a gfx_lib_check before it (the archive\'s backend must match GFX)' % (f, no))
dr = lines('mac/device_run.sh')
# TASK43 and its review: two GFX configurations: the TEST one (gfx_lines: vkd3d at warn, every line) and the PLAY one (gfx_play_lines: vkd3d
# at warn behind the runtime's de-duplicating filter when the signed app knows it, else vkd3d's default level, fixme).
# Each env line is written only inside `if [ "$GFX" = 1 ]`.
for env, want in (('KHNMC_GFX=vkd3dw', 2), ('VKD3D_DEBUG=warn', 2), ('VKD3D_SHADER_DEBUG=warn', 2), ('VKD3D_DEBUG=fixme', 1), ('VKD3D_SHADER_DEBUG=fixme', 1)):
    hits = [(no, t) for no, t in dr if 'env ' + env in t]
    check(len(hits) == want and all(re.search(r'if \[ "\$GFX" = 1 \]; then.*env ' + re.escape(env), t) is not None for _, t in hits),
          'mac/device_run.sh: `env %s` is not written on exactly %d line(s), each inside if [ "$GFX" = 1 ]' % (env, want))
test_l = [t for _, t in dr if re.search(r'\bgfx_lines\(\)\{ echo', t)]; playdef = [t for _, t in dr if re.search(r'\bgfx_play_lines\(\)\{ echo', t)]
check(len(test_l) == 1 and 'env VKD3D_DEBUG=warn' in test_l[0] and 'fixme' not in test_l[0] and 'KHNMC_GFX_VKD3D_LOG_REPEATS' not in test_l[0] and 'gfx_play_lines' not in test_l[0],
      'mac/device_run.sh: gfx_lines (the test configurations) must run vkd3d at warn with every line (no fixme, no log filter)')
check(len(playdef) == 1 and re.search(r'gfx_play_lines\(\)\{ echo "env KHNMC_GFX=vkd3dw"; if app_has_vklog; then echo "env VKD3D_DEBUG=warn"; echo "env VKD3D_SHADER_DEBUG=warn"; '
                                      r'echo "env KHNMC_GFX_VKD3D_LOG_REPEATS=[1-9][0-9]*"; else echo "env VKD3D_DEBUG=fixme"; echo "env VKD3D_SHADER_DEBUG=fixme"; fi;', playdef[0]) is not None,
      'mac/device_run.sh: gfx_play_lines must write vkd3d at warn ONLY with the runtime\'s log filter (KHNMC_GFX_VKD3D_LOG_REPEATS) and only when the signed app knows it (app_has_vklog), else fixme')
check(sum(1 for _, t in dr if 'echo "env KHNMC_GFX_VKD3D_LOG_REPEATS' in t) == 1, 'mac/device_run.sh: KHNMC_GFX_VKD3D_LOG_REPEATS is written outside gfx_play_lines')
check(any(re.search(r"^app_has_vklog\(\)\{ grep -a -q 'KHNMC_GFX_VKD3D_LOG_REPEATS' \"\$APP/KHNMC\"", t) for _, t in dr), 'mac/device_run.sh: app_has_vklog must look for the variable\'s name in the signed app')
play_l = [t for _, t in dr if '> "$OUT/gfx-play.conf"' in t]
check(len(play_l) == 1 and 'gfx_play_lines' in play_l[0] and re.search(r'(^|[; ])gfx_lines\b', play_l[0]) is None and 'quiet_lines' in play_l[0],
      'mac/device_run.sh: the GFX play configuration is not written from gfx_play_lines + quiet_lines (it must not use the test lines: vkd3d at warn without the filter floods the play log)')
nullplay = [t for _, t in dr if '> "$OUT/play.conf"' in t]
check(len(nullplay) == 1 and 'quiet_lines' in nullplay[0], 'mac/device_run.sh: the null play configuration does not use quiet_lines (log cap, rare frame line)')
ql = [t for _, t in dr if re.search(r'^quiet_lines\(\)', t)]
check(len(ql) == 1 and re.search(r'if app_has_logcap; then echo "logcap 16M"; fi', ql[0]) is not None and 'env KHNMC_GFX_FRAME_LINE_EVERY=600' in ql[0],
      'mac/device_run.sh: quiet_lines must write `logcap 16M` only when the signed app knows the directive (app_has_logcap) and the frame-line interval')
check(sum(1 for _, t in dr if 'echo "logcap' in t) == 1, 'mac/device_run.sh: `logcap` is written outside quiet_lines (an older app stops at an unknown conf directive)')
# the pipeline cache lines.  KHNMC_GFX_PIPECACHE_RESET=1 makes the app remove its cache file at EVERY start: it may only reach a route
# run's configuration (replaced by the play configuration when the run ends), never the play configuration that stays on the phone
reset_l = [t for _, t in dr if 'echo "env KHNMC_GFX_PIPECACHE_RESET=1"' in t]
check(len(reset_l) == 1 and re.search(r'^pc_lines\(\)\{ case "\$PIPECACHE" in fresh\) echo "env KHNMC_GFX_PIPECACHE_RESET=1";; off\) echo "env KHNMC_GFX_PIPECACHE=off";; esac; \}', reset_l[0]) is not None,
      'mac/device_run.sh: `env KHNMC_GFX_PIPECACHE_RESET=1` is not written exactly once, by pc_lines for PIPECACHE=fresh')
check(len(playdef) == 1 and re.search(r'(^|[; {])pc_lines\b', playdef[0]) is None and re.search(r'; pc_play_lines; \}; fi$', playdef[0]) is not None,
      'mac/device_run.sh: gfx_play_lines must take its cache line from pc_play_lines, never from pc_lines (the RESET line would stay on the phone and remove the cache at every icon launch)')
check(len(play_l) == 1 and re.search(r'(^|[; {])pc_lines\b', play_l[0]) is None and 'RESET' not in play_l[0], 'mac/device_run.sh: the GFX play configuration must not get pc_lines or a RESET line')
ppl = [t for _, t in dr if re.search(r'^pc_play_lines\(\)', t)]
check(len(ppl) == 1 and 'RESET' not in ppl[0] and re.search(r'^pc_play_lines\(\)\{ case "\$PIPECACHE" in off\) echo "env KHNMC_GFX_PIPECACHE=off";; on\) :;; '
                                                           r'\*\) \[ -f "\$STATE/pipecache-verified" \] \|\| echo "env KHNMC_GFX_PIPECACHE=off";; esac; \}$', ppl[0]) is not None,
      'mac/device_run.sh: pc_play_lines must turn the cache off unless PIPECACHE=on or $STATE/pipecache-verified exists, and never write RESET')
check(sorted(t.split('(')[0] for _, t in dr if 'echo "env KHNMC_GFX_PIPECACHE=off"' in t) == ['pc_lines', 'pc_play_lines'], 'mac/device_run.sh: `env KHNMC_GFX_PIPECACHE=off` is written outside pc_lines / pc_play_lines')
check(any(re.search(r'^\[ "\$PIPECACHE" != fresh \] \|\| \[ "\$MODE" = route \] \|\| bad ', t) for _, t in dr), 'mac/device_run.sh: PIPECACHE=fresh outside route mode is not refused (in play mode the RESET line would stay on the phone)')
check(any(re.search(r'^\[ "\$PIPECACHE" != on \] \|\| \[ "\$MODE" = play \] \|\| bad ', t) for _, t in dr), 'mac/device_run.sh: PIPECACHE=on outside play mode is not refused')
check(any(re.search(r'^case "\$PIPECACHE" in \'\'\|fresh\|off\|on\) ;; \*\) bad ', t) for _, t in dr), 'mac/device_run.sh: PIPECACHE is not limited to fresh, off, on or unset')
check(any(re.search(r'\[ -z "\$PIPECACHE" \] \|\| \[ "\$GFX" = 1 \] \|\| bad', t) for _, t in dr), 'mac/device_run.sh: PIPECACHE without GFX=1 is not refused')
pbx = open(os.path.join(D, 'KHNMC.xcodeproj', 'project.pbxproj')).read()
check(pbx.count('KHNMC_GFX_LDFLAGS = "";') == 2 and len(re.findall(r'"\$\(KHNMC_GFX_LDFLAGS\)",\n\t*\);', pbx)) == 2,
      'KHNMC.xcodeproj: KHNMC_GFX_LDFLAGS is not empty in both configurations, or not the last word of OTHER_LDFLAGS (python3 tools/ios/make_project.py)')
cm = ' '.join(t for _, t in lines('common.sh'))
check(re.search(r'\[ "\$\{GFX:-0\}" = 1 \] && IOS_ENV="\$IOS_ENV export GFX=1;"', cm) is not None, 'common.sh: GFX=1 is not handed to the Mac (IOS_ENV)')
# review X38 n=3: whether a file has the backend is asked through lib.sh's gfx_has only, which tells "nm could not read it" from "absent";
# `nm ... | grep -q` reads an unreadable file as "no backend" (a GFX=0 signing passed on an archive nm never read)
check(re.search(r'\bgfx_has\(\)', lib) is not None, 'mac/lib.sh: gfx_has is missing')
for f in sorted(x for x in os.listdir(os.path.join(D, 'mac')) if x.endswith('.sh')):
    bad = [no for no, t in lines('mac/' + f) if re.search(r'\bnm\s[^|;]*\|\s*grep\b', t)]
    check(not bad, 'mac/%s:%s: nm piped into grep: an unreadable file would read as "no backend" (use gfx_has)' % (f, ','.join(map(str, bad))))
# TASK50: CUTSCENES=story writes `env KHNMC_CUTSCENES=story` from cut_lines only, into all three configurations (play, GFX play, the
# test one), and only after app_has_cutscenes found the variable in the signed app (an older app would ignore it and play the stubs)
cut_def = [t for _, t in dr if re.search(r'^cut_lines\(\)', t)]
check(len(cut_def) == 1 and 'echo "env KHNMC_CUTSCENES=story"' in cut_def[0] and '[ "$CUTSCENES" != story ] ||' in cut_def[0],
      'mac/device_run.sh: cut_lines must write env KHNMC_CUTSCENES=story only when CUTSCENES=story')
check(sum(1 for _, t in dr if 'env KHNMC_CUTSCENES=story' in t and not t.lstrip().startswith('#')) == 1, 'mac/device_run.sh: env KHNMC_CUTSCENES=story is written outside cut_lines')
for tgt in ('> "$OUT/play.conf"', '> "$OUT/gfx-play.conf"'):
    hit = [t for _, t in dr if tgt in t]
    check(len(hit) == 1 and 'cut_lines' in hit[0], 'mac/device_run.sh: the configuration %s does not call cut_lines' % tgt)
check(any(re.search(r'\[ "\$DIFF" = none \] \|\| echo "env \$DV=\$DIFF"; cut_lines; fi', t) for _, t in dr), 'mac/device_run.sh: the test configuration does not call cut_lines')
script = '\n'.join(t for _, t in dr)
for required in ('[ "${SKIP_INSTALL:-0}" != 1 ] || bad "story mode', '[ -s "$CUTSCENES_PACK" ] || bad',
                 'shasum -a 256 < "$CUTSCENES_PACK"', 'grep -a -q "$pack_digest" "$APP/KHNMC"',
                 '&& app_has_cutscenes || { say', '--destination "Documents/$2"',
                 'cp_to "$CUTSCENES_PACK" data/story-events.khcs || { say "copy of the cutscene pack FAILED"; exit 1; }'):
    check(required in script, 'story deployment missing ' + required)
check(any(re.search(r'''case "\$CUTSCENES" in ''\|nmc\|story\) ;; \*\) bad ''', t) for _, t in dr), 'mac/device_run.sh: CUTSCENES must accept only nmc or story')

for f in fails:
    print('test_mac_rules: FAIL: ' + f)
print('test_mac_rules: %d checks, %d failed (%s)' % (n, len(fails), D))
sys.exit(1 if fails else 0)
