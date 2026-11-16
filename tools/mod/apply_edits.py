#!/usr/bin/env python3
"""Apply edit_list.json to the pristine kh2fm.exe -> patched PE image in scratch.
  usage: apply_edits.py [--exe PATH] [--out PATH] [--with-antiform]
The image keeps every pristine section byte-for-byte except the edit bytes; one new RW section '.nmc' (4 KiB virtual) is
appended after the Authenticode blob to hold the DLL's relocated tables (blow_gravity float, 18-entry world-symbol table,
relocated BSS symbol-instance block). The two BSS-only edits (script-syscall table pointer) are NOT file-backed; they are
emitted to runtime_init.json for the host layer to apply at load."""
import json, struct, sys, os, argparse
sys.dont_write_bytecode=True
HERE=os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0,HERE)
from pelib import *
ap=argparse.ArgumentParser(); ap.add_argument('--exe',default=EXE); ap.add_argument('--out',default=HERE+'/kh2fm.nmc.exe'); ap.add_argument('--with-antiform',action='store_true')
a=ap.parse_args()
E=json.load(open(HERE+'/edit_list.json'))
pe=PE(a.exe); assert sha256(pe.raw)==E['exe_sha256'],'exe differs from the pinned build'
raw=bytearray(pe.raw)
opt=pe.pe.OPTIONAL_HEADER
SA=opt.SectionAlignment; FA=opt.FileAlignment
last=pe.secs[-1]
nmc_rva=(opt.SizeOfImage+SA-1)//SA*SA; assert nmc_rva==opt.SizeOfImage
nmc_vsz=E['nmc_buffer']['size']
raw_off=(len(raw)+FA-1)//FA*FA
nmc_raw=bytearray(FA); nb=bytes.fromhex(E['nmc_buffer']['nonzero_hex']); nmc_raw[:len(nb)]=nb
# ---- headers
nt=pe.pe.DOS_HEADER.e_lfanew; nsec=pe.pe.FILE_HEADER.NumberOfSections
sh_off=nt+4+20+pe.pe.FILE_HEADER.SizeOfOptionalHeader
new_sh=sh_off+40*nsec
assert raw[new_sh:new_sh+40]==bytes(40) and new_sh+40<=opt.SizeOfHeaders,'no header slack for a section entry'
sh=struct.pack('<8sIIIIIIHHI',b'.nmc',nmc_vsz,nmc_rva,FA,raw_off,0,0,0,0,0xC0000040)
raw[new_sh:new_sh+40]=sh
struct.pack_into('<H',raw,nt+6,nsec+1)
struct.pack_into('<I',raw,nt+24+56,nmc_rva+nmc_vsz)                 # SizeOfImage
soi=nt+24+8; struct.pack_into('<I',raw,soi,opt.SizeOfInitializedData+FA)  # SizeOfInitializedData
raw+=bytes(raw_off-len(raw))+bytes(nmc_raw)
# ---- edits
applied=[];runtime=[]
def put(rva,new,old):
    fo=pe.file_off(rva); assert fo is not None
    assert bytes(raw[fo:fo+len(old)])==old,'pristine bytes mismatch at %x'%rva
    raw[fo:fo+len(new)]=new
final=[]
for e in E['edits']+(E['inactive_edits'] if a.with_antiform else []):
    rva=int(e['rva'],16); old=bytes.fromhex(e['old'])
    if e.get('cls')=='runtime_only_bss':
        runtime.append(dict(rva=e['rva'],old=e['old'],new=e['new'],note='heap pointer to the script syscall table in the DLL run; host layer sets the pointer to its own table')); continue
    if e.get('cls')=='rel32_to_new_section':
        tgt=nmc_rva+int(e['buffer_offset'],16); disp=tgt-(rva+4); new=struct.pack('<i',disp)
    else: new=bytes.fromhex(e['new'])
    put(rva,new,old); final.append(dict(rva=e['rva'],old=e['old'],new=new.hex(),cls=e.get('cls','inactive_antiform')))
open(a.out,'wb').write(raw)
json.dump(dict(nmc_section=dict(rva='0x%x'%nmc_rva,virtual_size='0x%x'%nmc_vsz,raw_offset='0x%x'%raw_off,raw_size='0x%x'%FA,characteristics='0xC0000040'),
    applied=final,runtime_only=runtime,with_antiform=a.with_antiform,out_sha256=sha256(raw)),open(HERE+'/applied.json' if not a.with_antiform else HERE+'/applied_with_antiform.json','w'),indent=1)
print('wrote',a.out,len(raw),'bytes; edits applied',len(final),'runtime-only',len(runtime),'nmc rva 0x%x'%nmc_rva)
