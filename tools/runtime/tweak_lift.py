"""Extract pinned lifted functions with configurable immediates. Never write into lifted/."""
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])
# (shard, function, basic block, literal, replacement, expected occurrences).
sites = [
    (51,"27a740","27a848","0x12ull","LD(u8,0x14027a84cull)",2),
    (51,"27a740","27a8d8","0x12ull","LD(u8,0x14027a8dcull)",2),
    (81,"3dc170","3dc334","t9=GET(float,288);","t9=kh_drop_roll_test(GET(float,288),GET(float,224),LD(u16,t25));",1),
    (76,"3a14b0","3a14b0","0x63ull","LD(u8,0x1403a14bcull)",2),
    (83,"3ecf30","3ed0b6","0x63ull","LD(u8,0x1403ed0bcull)",2),
    (83,"3f2030","3f2055","0x36ull","LD(u8,0x1403f205eull)",2),
]
for shard,func,block,imm in [(76,"3a5650","3a5713","3a5719"),(79,"3c0b50","3c0b8e","3c0b97"),
                            (81,"3dbea0","3dbf69","3dbf6f"),(85,"404d00","404d00","404d06"),
                            (85,"405030","4050f4","4050fb"),(85,"408520","4086fa","40870a")]:
    sites.append((shard,func,block,"0x6ull",f"LD(u8,0x140{imm}ull)",2))
sites.extend([(87,"421d60","421e02","0x64ull","LD(u8,0x140421e04ull)",1),
              (87,"421d60","421e0f","(u32)t14 * (u32)t12","(LD(u8,0x140421e16ull)==0x45 ? 0u : (u32)t14 * (u32)t12)",1)])
bodies = {}
for shard,func,block,old,new,count in sites:
    if func not in bodies:
        source = (root/f"shard_{shard:04}.c").read_text()
        start = source.index(f"void f_{func}(KCtx *g){{")
        bodies[func] = source[start:source.index("\nvoid f_",start+1)]
    s = bodies[func]
    a = s.index(f"\nL{block}:")
    b = s.index("\n}",a)
    part = s[a:b]
    assert part.count(old)==count,(func,block,old,part.count(old))
    bodies[func] = s[:a]+part.replace(old,new)+s[b:]
print('#include "khrt.h"\nextern float kh_drop_roll_test(float,float,u16);')
calls = sorted(set(re.findall(r"(f_[0-9a-f]+)\(g\)", "".join(bodies.values()))))
for name in calls:
    print(f"void {name}(KCtx *g);")
for func,body in bodies.items():
    name={"3dc170":"kh_drop_roll","3ecf30":"kh_level_award"}.get(func,"f_"+func)
    print(body.replace(f"void f_{func}(",f"void {name}("))
