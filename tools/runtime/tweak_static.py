"""Route shipped static edits through the same mapped instruction bytes the INI loader selects."""
import pathlib
import re
import sys

body, table = (pathlib.Path(p).read_text() for p in sys.argv[1:])
first = {}
for address, func, option, old in re.findall(r'\{0x(\w+),\d+,0,\d+,0x(\w+),"(\w+)",\{0x(\w+)', table):
    if option == "blow_gravity":
        continue
    first.setdefault(func, (address, old))
for func, (address, old) in first.items():
    if func == "401d60":
        # Two drive branches and one Pride Lands immediate share a body.
        a = body.index("void f_401d60(KCtx *g){")
        b = body.index("\n}", body.index("return;", body.index("L401f1e:", a)))
        part = body[a:b]
        for pc in ("401ec4","401f18"):
            pat = rf'(0x140{pc}ull\)\);\n[^\n]*calc_cond\(\(u64\)\()\(u64\)0x2ull'
            part, n = re.subn(pat, rf'\g<1>(LD(u8,0x140{int(pc,16)+1:x}ull)==0x85?5:2)', part)
            assert n == 1, pc
        assert part.count('0x4dull') == 2
        body = body[:a]+part.replace('0x4dull','LD(u8,0x140401ee9ull)')+body[b:]
        continue
    match = re.search(rf'void (?:mod_)?f_{func}\(KCtx \*g\)\{{', body)
    assert match, func
    branch = f'\nif(LD(u8,0x140{address}ull)==0x{old}){{orig_f_{func}(g);return;}}'
    body = body[:match.end()]+branch+body[match.end():]
assert body.count("0x74420ull")==2
print(body.replace("0x74420ull","LD(u32,0x1403b1788ull)"))
