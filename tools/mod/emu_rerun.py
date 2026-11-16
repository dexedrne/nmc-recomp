"""Re-run KH2Tweaks_Refactor.dll DllMain in Unicorn against the STATIC pristine exe bytes (no game run) with the
shipped ini, and dump (a) the exe-image diff, (b) the DLL-allocated buffers that rel32 patches point at.
The only deviation from the shipped ini: worldpoint_amount 'on' -> 54 (the DLL's stoi wrapper 0x2ab00 catches the
std::invalid_argument and returns its default 0x36 = 54; the emulator has no C++ EH, so we feed the default directly)."""
import sys, json, pickle, struct, os
S5='/home/user/projects/kh2-nmc-analysis/scratch/S5-mod-hooks'
sys.path.insert(0,S5); sys.dont_write_bytecode=True
from emu import *
ini=sys.argv[1]; out=sys.argv[2]
e=Emu(MOD+'KH2Tweaks_Refactor.dll',ini,verbose=False,cwd=MOD)
sc=json.load(open(S5+'/scan_emu.json'))
e.add_shortcuts({x['scan']:(x['g_res'],x['res_abs'] if x['res_rva'] else 0) for x in sc})
ep=e.dll_base+e.dll.pe.OPTIONAL_HEADER.AddressOfEntryPoint
attach=[]
def on_attach(uc,addr,size,ud):
    rcx=e.r(UC_X86_REG_RCX); rdx=e.r(UC_X86_REG_RDX)
    tgt=struct.unpack('<Q',bytes(uc.mem_read(rcx,8)))[0]
    attach.append((tgt-EXE_BASE,rdx-0x180000000))
e.uc.hook_add(UC_HOOK_CODE,on_attach,begin=e.dll_base+0x35d40,end=e.dll_base+0x35d41)
r=e.run(ep,(e.dll_base,1,0,0))
d=e.diff()
regs={}
for lo,hi in getattr(e,'extra_regions',[]):
    regs[lo]=bytes(e.uc.mem_read(lo,hi-lo))
pickle.dump(dict(ret=r,diff=d,attach=attach,extra=regs,out=e.out,ini_reads=e.ini_reads),open(out,'wb'))
print('ret',r,'regions',len(d),'bytes',sum(len(o) for s,o,n in d),'attach',len(attach),'extra',[(hex(k),len(v)) for k,v in regs.items()])
