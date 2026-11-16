#!/usr/bin/env python3
"""usage: dis.py START END [orig|nmc]   (hex vaddrs, END exclusive) -> annotated MIPS (R5900) disassembly.
j/jal targets are annotated with function names from ../kh2-decomp/layout.tsv (FUN_xxxx = unnamed)."""
import sys, struct, subprocess, bisect, os
D=os.path.dirname(os.path.abspath(__file__))
s,e=int(sys.argv[1],16),int(sys.argv[2],16); which=sys.argv[3] if len(sys.argv)>3 else 'orig'
data=open(f'{D}/{which}/SLPM_666.75','rb').read()
segs=[(0x100000,0x1000,0x27a800),(0x1e50000,0x281ef4,0x400)] if which=='nmc' else [(0x100000,0x1000,0x27a800)]
fn=[]
for l in open(f'{D}/../kh2-decomp/layout.tsv'):
    p=l.rstrip('\n').split('\t')
    try: fn.append((int(p[0],16),int(p[1],16),p[3]))
    except: pass
fn.sort(); st=[f[0] for f in fn]
def name(va):
    i=bisect.bisect_right(st,va)-1
    return fn[i][2] if i>=0 and fn[i][0]<=va<fn[i][1] else ('injected' if 0x1e50000<=va<0x1e50400 else '?')
def off(va):
    for b,o,sz in segs:
        if b<=va<b+sz: return o+va-b
    raise SystemExit(f'vaddr {va:#x} not in file-backed segment')
words=[struct.unpack('<I',data[off(v):off(v)+4])[0] for v in range(s,e,4)]
txt=' '.join('0x%02x 0x%02x 0x%02x 0x%02x'%tuple(struct.pack('>I',w)) for w in words)
out=[x.strip() for x in subprocess.run(['llvm-mc','--disassemble','--triple=mips64'],input=txt,capture_output=True,text=True).stdout.splitlines() if x.strip() and not x.strip().startswith('.')]
for i,w in enumerate(words):
    va=s+4*i; asm=out[i] if i<len(out) else '??'
    if asm.startswith(('jal\t','j\t')):
        t=int(asm.split('\t')[1]); asm+=f'   ; -> {t:#x} <{name(t)}>'
    print(f'{va:08x}: {w:08x}  {asm}')
