#!/usr/bin/env python3
"""mod_update_check_mutants.py - mutation check of `tools/mod_update_check.py --selftest` (task 34).

  mod_update_check_mutants.py [--tool tools/mod_update_check.py] [--keep]

Each mutation below breaks ONE rule of the comparison (a textual replacement that must occur exactly once in the tool).  For each, a
copy of the tool (with the two modules it imports, tools/bdxscan.py and tools/mod_dll_slots.py) is made in a temporary directory, the
mutation is applied, and `--selftest` runs; the selftest must FAIL (exit status not 0).  The unmutated copy must PASS first.
Exit status: 0 when every mutant is killed, 1 when one survives or a mutation text is not found (a stale list is an error, not a skip).
"""
import argparse, os, shutil, subprocess, sys, tempfile

MUTANTS = [
    ('line endings: never "line endings only"', "        if o[3] and n[2] and o[3] == n[2]:\n            eol.append(rel)",
     "        if False:\n            eol.append(rel)"),
    ('files: removed files not seen', "    removed = sorted(set(old) - set(new))", "    removed = []"),
    ('files: added files not seen', "    added = sorted(set(new) - set(old))", "    added = []"),
    ('files: content compared by size only', "        if o[1] == n[0] and o[2] == n[1]:\n            continue",
     "        if o[1] == n[0]:\n            continue"),
    ('unknown kinds are not loud', "                if k in LOUD_KINDS:", "                if False:"),
    ('ini: changed values ignored', "            if ov == nv:\n                continue", "            if True:\n                continue"),
    ('ini: inline #comment stripped (not what the API does)', "            out.append([sec, k.strip(), v.strip()])",
     "            out.append([sec, k.strip(), v.split('#')[0].strip()])"),
    ('ini: boolean reader takes "no" as on', "    return v.lower() in ('on', 'true', '1')", "    return v.lower() in ('on', 'true', '1', 'no')"),
    ('ini: last occurrence wins', "        d.setdefault(((sec or '').lower(), k.lower()), v)", "        d[((sec or '').lower(), k.lower())] = v"),
    ('ini: a guarded option switched on is not loud', "                if bool_on(nv):", "                if False:"),
    ('detours: "moved" never recognised', "        elif oh.get('norm') and oh.get('norm') == nh.get('norm'):", "        elif False:"),
    ('detours: callee change not seen', "            st = 'moved' if (not oh.get('deep') or oh.get('deep') == nh.get('deep')) else 'callee'",
     "            st = 'moved'"),
    ('detours: new detours dropped', "            out.append(dict(status='new', old=None, new=r))", "            pass"),
    ('detours: no fallback on the handler\'s code', "        for field in ('deep', 'norm'):", "        for field in ():"),
    ('detours: guarded detour re-audited although its option is off',
     "            if gate in GUARDED_OPTIONS and not option_on(A, (o or {}).get('ini'), gate):", "            if gate in GUARDED_OPTIONS and False:"),
    ('detours: guarded detour never re-audited', "            if gate in GUARDED_OPTIONS and not option_on(A, (o or {}).get('ini'), gate):",
     "            if gate in GUARDED_OPTIONS:"),
    ('scripts: unimplemented calls not reported', "    for t in sorted(nu - impl):", "    for t in sorted(set()):"),
    ('scripts: table 3 not the one scanned', "                if t != 3:\n                    continue", "                if t != 4:\n                    continue"),
    ('files: case-only renames not recognised', "    case = [(lo_rm[a.lower()], a) for a in added if a.lower() in lo_rm]", "    case = []"),
    ('mod.yml: non-copy methods accepted', "    bad = {m: c for m, c in meth.items() if m != 'copy'}", "    bad = {}"),
    ('installed changes do not ask for a re-bake', "                    installed_change = True", "                    installed_change = installed_change"),
    ('mod.yml: an internal source counts as a plain asset', "set(a['sources'][0]) == {'name'}", "True"),
    ('KH2Moose: scd_priority bit 2 not checked', "iv is not None and iv & 2", "iv is not None and iv & 4"),
    ('line endings only: port steps asked for', "        return rep, False                  # line endings only: reported, no port step",
     "        pass"),
    ('kinds: HD files not recognised', "    ('hd', r'^remastered/'),", "    ('hd', r'^remastered_x/'),"),
    ('kinds: root-level game files unknown', "    ('system-table', r'^[0-9]{2}[a-z_]+\\.(bin|bar|bdx)$'),",
     "    ('system-table', r'^[0-9]{2}[a-z_]+\\.(bin|bdx)$'),"),
    ('ini: an out-of-scope option switched on is not loud', "            if k in OUT_OF_SCOPE_OPTIONS and bool_on(nv):",
     "            if False:"),
    ('ini: static-edit keys not classified', "    if (fname, k) in edit_keys:\n        cls.append('static-edit')",
     "    if False:\n        cls.append('static-edit')"),
    # ---- task 34 review findings ----------------------------------------------------------------------------------------------------
    ('leaf callee: a code address without .pdata stays the bare token T', "            if leaf_ref is not None and leaf_ref(rva):",
     "            if False:"),
    ('leaf callee: never given leaf bounds', "        self.leaf[rva] = self.leaf_bounds(rva)\n        return True", "        return False"),
    ('detours: identical handler bytes are "same" whatever they reach',
     "                (not oh.get('deep') or not nh.get('deep') or oh['deep'] == nh['deep']):", "                True:"),
    ('ini keys: identical loader bytes are "same" whatever they reach',
     "o.get('begin') == n.get('begin') and o.get('deep') == n.get('deep'):\n        return 'same'",
     "o.get('begin') == n.get('begin'):\n        return 'same'"),
    ('ini keys: a changed loader is not a step', "            if st != 'same':\n                groups.setdefault((ob, nb, st), []).append(k)",
     "            if False:\n                groups.setdefault((ob, nb, st), []).append(k)"),
    ('ini keys: a loader that only moved counts as the same', "        return 'moved' if o.get('deep') == n.get('deep') else 'callee'",
     "        return 'same' if o.get('deep') == n.get('deep') else 'callee'"),
    ('ini keys: a key the new DLL no longer names is not a step', "        if o.get('named') and not n.get('named'):", "        if False:"),
    ('ini keys: a changed DLL default is not a step',
     "        if (o.get('default_type'), o.get('default')) != (n.get('default_type'), n.get('default')):", "        if False:"),
    ('ini keys: a detour gate read by changed code is no [hooks] step', "        if any(key_is_gate(k) for k in ks):\n            rep.step('hooks', reason)",
     "        if False:\n            rep.step('hooks', reason)"),
    ('ini keys: a not-read key named by the new DLL is not loud', "            if first and c.startswith('not-read') and k.lower() in ks:",
     "            if False:"),
    ('case: DLLs compared by exact name only', "        if rel.lower() in touched:", "        if rel in changed + added + removed:"),
    ('case: DLLs opened by exact name only', "        a = actual(rel)\n        p = os.path.join(mod, a) if a else None",
     "        a = rel if os.path.isfile(os.path.join(mod, rel)) else None\n        p = os.path.join(mod, a) if a else None"),
    ('case: renames of exactly-named files ignored', "        how = EXACT_NAME_USERS.get(a_.lower())", "        how = None"),
    ('gummi: not listed after a bake', "    if 'bake' in rep.steps:\n        rep.step('gummi',", "    if False:\n        rep.step('gummi',"),
    ('gummi: a shipped wm00.ard is not a gummi input', "        if re.match(r'^ard/[^/]+/wm00\\.ard$', rel, re.I):\n            rep.step('gummi'",
     "        if False:\n            rep.step('gummi'"),
    ('items: an unported effect kind is no step', "    for t, k, c in unported_item_kinds(n):", "    for t, k, c in []:"),
    ('items: every item type counts as an item the player uses', "        if int(t) in ITEM_USE_TYPES:", "        if True:"),
    ('items: the kind read from the wrong byte', "            typ, kind = d[8 + 24 * i + 2], d[8 + 24 * i + 5]",
     "            typ, kind = d[8 + 24 * i + 2], d[8 + 24 * i + 4]"),
    ('extra-file probe: a probed file is not loud', "        if re.match(EXTRA_PROBE_RE, rel.split('/')[-1], re.I):", "        if False:"),
    ('pin: slots of another build pass uncompared', "        if rows and ncmp != nact:", "        if False:"),
    ('pin: an inline site off the spliced address passes', "                if r.get('exe') != site:", "                if False:"),
    ('pin: the Makefile may check another expectation file', "        if mk_exp != sf:", "        if False:"),
    ('pin: an absent ini key is skipped (the old rule)', "                if v is None:\n                    out.append('mod_config.h difficulty",
     "                if v is None:\n                    continue\n                    out.append('mod_config.h difficulty"),
    ('pin: the ini value used although the DLL does not read the key', "            if not r or not r.get('named'):\n                return None, 'KH2Tweaks",
     "            if False:\n                return None, 'KH2Tweaks"),
    ('pin: the decoded DLL default not used for an absent key', "            if r.get('default') is None:", "            if True:"),
    ('pin: Moose / Neverland functions not found again pass', "        for p in (fp['dll'].get(rel) or {}).get('problems', []):\n            out.append(p)",
     "        for p in []:\n            out.append(p)"),
    ('pin: a stale gummi overlay input passes', "            if prog and go.get('IN_PROG') != prog[2]:", "            if False:"),
    ('pin: an unported pinned item kind passes', "    for t, k, c in unported_item_kinds(fp.get('item_kinds')):", "    for t, k, c in []:"),
]


def run(tool_dir):
    p = subprocess.run([sys.executable, '-I', os.path.join(tool_dir, 'mod_update_check.py'), '--selftest'], capture_output=True, text=True,
                       cwd=tool_dir, timeout=600)
    return p.returncode, (p.stdout + p.stderr).strip().split('\n')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--tool', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'mod_update_check.py'))
    ap.add_argument('--keep', action='store_true')
    a = ap.parse_args()
    src_dir = os.path.dirname(os.path.abspath(a.tool))
    src = open(a.tool).read()
    tmp = tempfile.mkdtemp(prefix='mod_update_check_mutants_')
    bad = 0
    try:
        for f in ('bdxscan.py', 'mod_dll_slots.py'):
            shutil.copy(os.path.join(src_dir, f), tmp)
        dst = os.path.join(tmp, 'mod_update_check.py')
        open(dst, 'w').write(src)
        rc, out = run(tmp)
        print('unmutated: exit %d, %s' % (rc, out[0] if out else ''))
        if rc != 0:
            print('the unmutated selftest fails: fix that first')
            return 1
        for i, (what, old, new) in enumerate(MUTANTS, 1):
            n = src.count(old)
            if n != 1:
                print('%2d. %-62s MUTATION TEXT FOUND %d TIMES (stale list)' % (i, what, n))
                bad += 1
                continue
            open(dst, 'w').write(src.replace(old, new))
            rc, out = run(tmp)
            first_fail = next((l.strip() for l in out if 'FAIL' in l or 'Error' in l), out[-1] if out else '')
            killed = rc != 0
            bad += not killed
            print('%2d. %-62s %s  (%s)' % (i, what, 'killed' if killed else 'SURVIVED', first_fail[:110]))
    finally:
        if not a.keep:
            shutil.rmtree(tmp, ignore_errors=True)
    print('mutants: %d, killed %d, survived or stale %d' % (len(MUTANTS), len(MUTANTS) - bad, bad))
    return 0 if bad == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
