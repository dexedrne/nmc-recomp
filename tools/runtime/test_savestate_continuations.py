"""Linux microtest. Use heavy.sh python3 tools/test_savestate_continuations.py run-V58/micro."""
from hashlib import sha256
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
source = (root / 'lifted/shards/shard_0027.c').read_text()
functions = []
for name in ('160560', '160530', '1568b0'):
    body = re.search(r'void f_' + name + r'\(KCtx \*g\)\{.*?\n\}\n\}', source, re.S)
    if not body:
        raise ValueError(f'missing lifted function {name}')
    code = body[0]
    # These call blocks end in an unconditional goto. Each successor declares all
    # of its temporaries locally. Its only live inputs are guest state and memory.
    if name != '160560':
        callee, label = {'160530': ('160560', '16053e'), '1568b0': ('160530', '1568be')}[name]
        call = f'f_{callee}(g);\ngoto L{label};'
        if code.count(call) != 1 or code.count(f'L{label}:{{') != 1:
            raise ValueError(f'unsupported continuation in {name}')
        code = code.replace('KCtx *g){', f'KCtx *g){{\nif(resume_label==0x{label}) goto L{label};', 1)
        if name == '160530':
            code = code.replace(call, 'leaf_yield(g);\n' + call)
    functions.append(code)

generated = '\n'.join(functions)
abi = sha256(generated.encode()).digest()
(out / 'continuations.inc').write_text(
    'static const unsigned char lift_id[32]={' + ','.join(map(str, abi)) + '};\n' + generated)
exe = out / 'micro'
subprocess.run(['cc', '-O2', '-std=gnu11', '-DKH_XLATE_MODE=2', '-I'+str(root/'lifter-rt'),
                '-I'+str(out), str(root/'tools/savestate_continuations_test.c'), '-lcrypto', '-o', exe], check=True)
for action in ('save', 'load'):
    subprocess.run([exe, action], cwd=out, check=True)
if (out/'save.end').read_bytes() != (out/'load.end').read_bytes():
    sys.exit('continuation mismatch')
slot = out / 'micro.slot'
original = slot.read_bytes()
try:
    missing_outer = bytes(4) + original[68:]
    cases = {
        'length': original[:-1],
        'lift-mismatch': bytes(32) + original[32:],
        'checksum': original[:32] + bytes(32) + original[64:],
        'caller-chain': original[:32] + sha256(missing_outer).digest() + missing_outer,
    }
    for reason, damaged in cases.items():
        slot.write_bytes(damaged)
        result = subprocess.run([exe, 'load'], cwd=out, stderr=subprocess.PIPE, text=True)
        if result.returncode != 3 or result.stderr.strip() != f'micro-slot-refused: {reason}':
            sys.exit(f'{reason} not detected')
        print(result.stderr.strip())
finally:
    slot.write_bytes(original)
print('nested return, suspended fiber local, fresh bias, short slot and missing caller: passed')
