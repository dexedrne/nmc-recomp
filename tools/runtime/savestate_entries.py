"""Emit alternate entries at the lifted return sites of the game thread and field fibers."""
import re
import sys
from pathlib import Path

# Shard, containing function, successor, instruction that yielded/called.
entries = (
    (19, 'fddd0', 'fddde', 'kh_dispatch(g,t6);'),
    (20, 'fe930', 'feade', 'f_fdd70(g);'),
    (27, '1548e0', '154a1b', 'f_105e10(g);'),
    (26, '14fd60', '14fdfe', 'kh_dispatch(g,t2);'),
    (26, '14eb70', '14ecde', 'f_14fc20(g);'),
    (22, '121d90', '121f09', 'kh_dispatch(g,t4);'),
    (23, '127ed0', '127ed9', 'f_121d90(g);'),
    (24, '135470', '13552f', 'rt_imp_kernel32__SwitchToFiber(g);'),
    (27, '1572b0', '157334', 'rt_hook_fiber_yield_150060(g);'),
    (58, '2cb5d0', '2cbc2a', 'rt_hook_fiber_yield_150060(g);'),
    (58, '2cc180', '2cc31a', 'rt_hook_fiber_yield_150060(g);'),
    (26, '150310', '1503c8', 'rt_hook_fiber_yield_150060(g);'),
    (38, '1e0140', '1e0351', 'rt_hook_fiber_yield_150060(g);'),
    (27, '154b50', '154d8b', 'rt_hook_fiber_yield_150060(g);'),
    (26, '14f650', '14f65b', 'kh_dispatch(g,t7);'),
    (24, '135220', '13522e', 'kh_dispatch(g,t6);'),
)

print('#include "khrt.h"')
for shard, func, label, instruction in entries:
    source = (Path(sys.argv[1]) / f'shard_{shard:04d}.c').read_text()
    start = f'void f_{func}(KCtx *g){{'
    body = source.split(start, 1)[1].split('\n}\n}', 1)[0] + '\n}\n}'
    if f'{instruction}\ngoto L{label};\n}}\nL{label}:{{' not in body:
        raise ValueError(f'f_{func} has no complete call boundary at {label}')
    # Every successor declares fresh temporaries. The compiler checks that no
    # value from the abandoned caller's C scope is needed after the jump.
    for callee in sorted(set(re.findall(r'\b((?:f_|rt_)[A-Za-z_0-9]+)\(g\);', body))):
        print(f'void {callee}(KCtx *g);')
    if func == 'fe930':
        # Inspect the request list only after the cut has had a chance to park
        # the worker. Each block has fresh temporaries in the generated lift.
        print('void kh_cut_before_request(KCtx *,int);')
        for site in ('fe990', 'fea00'):
            boundary = f'L{site}:{{'
            if body.count(boundary) != 1:
                raise ValueError(f'missing stream request boundary {site}')
            body = body.replace(boundary, boundary + f' kh_cut_before_request(g,0x{site});')
        print('void kh_cut_stream_steps(KCtx *g,int pc){')
        print('switch(pc){ case 0xfeade: goto Lfeade; case 0xfe990: goto Lfe990; case 0xfea00: goto Lfea00; }')
        print(body)
    else:
        print(f'void kh_resume_{label}(KCtx *g){{ goto L{label};' + body)
