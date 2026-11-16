#!/usr/bin/env python3
"""Derive the SillyBillyMod1 static exe edits (as shipped) from the DLL's own init code + shipped ini.
Inputs : pristine kh2fm.exe, emu_runs/wp54.pkl (made by emu_rerun.py: DLL DllMain run in Unicorn on the static image),
         the shipped ini files, S5 patches.json (labels only; cross-checked)
Output : edit_list.json  (89 static edits + 9 inactive AntiForm edits + runtime-only entries)
"""
import json, pickle, struct, sys, os, re, hashlib
sys.dont_write_bytecode=True
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from pelib import *
HERE=os.path.dirname(os.path.abspath(__file__))
S5='/home/user/projects/kh2-nmc-analysis/scratch/S5-mod-hooks/'
MODDLL='/home/user/projects/SillyBillyMod1/dll/'
pe=PE()
assert sha256(pe.raw).startswith('9002b2de6a1f91a7'),'exe is not the pinned build'
# ---- shipped ini with Windows GetPrivateProfileString semantics: first match wins, value trimmed, ';' comments only
def read_ini(fn):
    d={};cur=None
    for line in open(MODDLL+fn,encoding='latin1').read().splitlines():
        s=line.strip()
        if not s or s[0]==';': continue
        if s.startswith('['): cur=s[1:s.find(']')].lower(); d.setdefault(cur,{}); continue
        if '=' in s and cur is not None:
            k,v=s.split('=',1); d[cur].setdefault(k.strip().lower(),v.strip())
    return d
INI={f:read_ini(f) for f in os.listdir(MODDLL) if f.endswith('.ini')}
def ini_get(f,k): return INI[f]['settings'].get(k.lower())
def bool_read(v):   # the DLL's boolean reader: lowercase, exactly 'on','true' or '1' (S5 finding, verified by the nocomment run)
    return v is not None and v.lower() in ('on','true','1')

d=pickle.load(open(HERE+'/emu_runs/wp54.pkl','rb')); dn=pickle.load(open(HERE+'/emu_runs/nocomment.pkl','rb'))
attach=d['attach']; att=[t for t,h in attach]
def is_det(s): return any(t<=s<t+16 for t in att)
static=[(s,o,n) for s,o,n in d['diff'] if not is_det(s)]
detour=[(s,o,n) for s,o,n in d['diff'] if is_det(s)]
assert len(d['diff'])==126 and sum(len(o) for s,o,n in d['diff'])==403
assert len(static)==89 and len(detour)==37 and len(att)==35
# inactive (AntiForm) = in nocomment but not in as-shipped
shipped_set={s for s,o,n in d['diff']}
inactive=[(s,o,n) for s,o,n in dn['diff'] if s not in shipped_set and not is_det(s)]
assert len(inactive)==9 and sum(len(o) for s,o,n in inactive)==12
# ---- labels: S5 patches.json entries 35..52 (label reuse; set equality asserted)
P=json.load(open(S5+'patches.json'))
label={}
for idx in range(35,53):
    x=P[idx]
    for r in x['regions']: label[int(r['rva'],16)]=(idx,x)
assert set(label)==set(s for s,o,n in static)
GROUP_NAME={35:'disable_subjective',36:'shop_restrict_fix',37:'apply_pridelands_patches',38:'enable_negative_stats',39:'apply_worldsymbol_patch',
 40:'aerial_dodge_patch',41:'remove_modifier_attack_flag',42:'init_save_sizes(unconditional)',43:'disable_drive_restrictions',44:'patch_limits',
 45:'disable_genie_tink_special',46:'uncap_form_level',47:'blow_gravity',48:'dualwield_forms(tables)',49:'form_growth(valor/wisdom/limit/master/final)',
 50:'init_table_718160(unconditional)',51:'camera_patches',52:'script_syscall_table_ptr(unconditional)'}
OPT_INI={35:[('KH2_Tweaks_Subjective.ini','disable_subjective')],36:[('KH2_Tweaks_Shop.ini','shop_restrict_fix')],37:[('KH2_Tweaks_Forms.ini','apply_pridelands_patches')],
 38:[('KH2_Tweaks_Combat.ini','enable_negative_stats')],39:[('KH2_Tweaks_Worlds.ini',k) for k in ('apply_worldsymbol_patch','symbol_zz','symbol_es','symbol_di','symbol_po','symbol_wi','symbol_tr')],
 40:[('KH2_Tweaks_Combat.ini','aerial_dodge_patch')],41:[('KH2_Tweaks_Combat.ini','remove_modifier_attack_flag')],42:[],43:[('KH2_Tweaks_Forms.ini','disable_drive_restrictions')],
 44:[('KH2_Tweaks_Limits.ini','patch_limits')],45:[('KH2_Tweaks_Combat.ini','disable_genie_tink_special')],46:[('KH2_Tweaks_Forms.ini','uncap_form_level')],
 47:[('KH2_Tweaks_Combat.ini','blow_gravity')],48:[('KH2_Tweaks_Forms.ini','dualwield_forms')],
 49:[('KH2_Tweaks_Forms.ini',k) for k in ('valor_growth','wisdom_growth','limit_growth','master_growth','final_growth')],
 50:[],51:[('KH2_Tweaks_Camera.ini','camera_patches_outdoors'),('KH2_Tweaks_Camera.ini','camera_patches_indoors')],52:[]}
# ---- classification
REL32={0x3901b1,0x3c51a0,0x3c5562,0x3c5878,0x3c590d,0x3c5ad1,0x3c5ad8,0x422835}   # rip-relative disp32 repointed to a DLL-allocated buffer
BSS={0x7534a8,0x7534ac}                                                         # runtime heap pointer into the BSS part of .data
NMC_RVA=None   # fixed by apply_edits (new section); buffer-relative offsets recorded here
BUF_BASE=0x13fff0000
def insn_text(img,rva):
    i=pe.insn_at(rva,img)
    return None if i is None else ('%x %s %s [%s]'%(i.address,i.mnemonic,i.op_str,bytes(img[i.address:i.address+i.size]).hex()))
def patched_img(s,n):
    b=bytearray(pe.img); b[s:s+len(n)]=n; return bytes(b)
edits=[]
for s,o,n in sorted(static):
    idx,x=label[s]
    assert pe.img[s:s+len(o)]==o,'old bytes mismatch at %x'%s
    sec=pe.sec(s); fo=pe.file_off(s)
    cls='literal'
    rec=dict(rva='0x%x'%s,va='0x%x'%(BASE+s),section=sec,file_offset=None if fo is None else '0x%x'%fo,length=len(o),old=o.hex(),new=n.hex())
    if s in REL32:
        cls='rel32_to_new_section'
        tgt=s+4+struct.unpack('<i',n)[0]+(BASE-0)   # absolute VA in emulation
        off=(BASE+s+4+struct.unpack('<i',n)[0])-BUF_BASE
        rec['old_target_rva']='0x%x'%(s+4+struct.unpack('<i',o)[0])
        rec['buffer_offset']='0x%x'%off
        rec['emu_new']=n.hex(); rec['new']=None   # final bytes computed by apply_edits once the section RVA is known
    elif s in BSS:
        cls='runtime_only_bss'
        rec['file_offset']=None
    rec.update(cls=cls,group=GROUP_NAME[idx],patches_json_index=idx,
        source_ini=[dict(file=f,key=k,shipped=ini_get(f,k),read_as_on=bool_read(ini_get(f,k))) for f,k in OPT_INI[idx]],
        effect=x['effect'],confidence=x['confidence'])
    if sec=='.text':
        rec['old_insn']=insn_text(pe.img,s)
        if s not in REL32: rec['new_insn']=insn_text(patched_img(s,n),pe.insn_at(s).address if pe.insn_at(s) else s)
    edits.append(rec)
# ---- the .nmc buffer content derived from ini + exe (checked against the DLL's own buffer from the emulation)
def pack_sym(v):
    a=[int(t) for t in v.split(',')]
    return struct.pack('<IBBBB',a[0],a[1],a[2],a[3],a[4])
vanilla=bytes(pe.img[0x5c07c0:0x5c07c0+12*8])
syms=b''.join(pack_sym(ini_get('KH2_Tweaks_Worlds.ini','symbol_'+k)) for k in ('zz','es','di','po','wi','tr'))
bg=struct.unpack('<f',struct.pack('<f',float(ini_get('KH2_Tweaks_Combat.ini','blow_gravity'))))
nmc=bytearray(0x1000)
nmc[0:4]=struct.pack('<f',float(ini_get('KH2_Tweaks_Combat.ini','blow_gravity')))
nmc[0x10:0x10+len(vanilla)+len(syms)]=vanilla+syms
emu_buf=d['extra'][BUF_BASE]
# DLL layout: 12 vanilla entries, then zz,es then three zero entries then tr  -> same as vanilla+syms ordering
assert bytes(nmc[:0xb0])==emu_buf[:0xb0],'derived .nmc buffer differs from the DLL buffer'
assert emu_buf[0xb0:]==bytes(len(emu_buf)-0xb0)
# ---- inactive
inact=[]
for s,o,n in sorted(inactive):
    i=pe.insn_at(s)
    inact.append(dict(rva='0x%x'%s,old=o.hex(),new=n.hex(),section=pe.sec(s),file_offset='0x%x'%pe.file_off(s),length=len(o),
        old_insn=insn_text(pe.img,s),new_insn=insn_text(patched_img(s,n),i.address if i else s),
        source_ini=[dict(file='KH2_Tweaks_AntiForm.ini',key=k,shipped=ini_get('KH2_Tweaks_AntiForm.ini',k),read_as_on=bool_read(ini_get('KH2_Tweaks_AntiForm.ini',k))) for k in ('antiform_tweaks','disable_random_anti','disable_ability_anti')],
        reason='NOT APPLIED as shipped: inline "#" comment makes the value "on #..." which the DLL bool reader treats as false'))
out=dict(
 exe_sha256=sha256(pe.raw),exe_size=len(pe.raw),image_base='0x140000000',
 summary=dict(emulated_diff_regions=126,emulated_diff_bytes=403,detour_hook_regions=len(detour),detour_hooks=len(att),
   static_edits=len(edits),static_edit_bytes=sum(e['length'] for e in edits),
   by_class={c:sum(1 for e in edits if e['cls']==c) for c in set(e['cls'] for e in edits)},
   inactive_antiform_edits=len(inact)),
 nmc_buffer=dict(emu_va='0x%x'%BUF_BASE,size=0x1000,nonzero_hex=bytes(nmc[:0xb0]).hex(),
    layout={'0x00':'float32 blow_gravity (ini blow_gravity=0.81632656)','0x10':'world-symbol table: 12 vanilla 8-byte entries copied from exe RVA 0x5c07c0, then zz,es,di,po,wi,tr from ini (u32 OBJ_ID,u8 World,u8 bgGroup,u8 Course,u8 pad)',
            '0x100':'zero-filled relocated copy of the BSS symbol-instance block (vanilla at exe RVA 0x2a23a80; sites 0x3c51a0/0x3c5ad1 -> +0x100, 0x3c5562/0x3c5878/0x3c5ad8 -> +0x118, 0x3c590d -> +0x148)'}),
 edits=edits,inactive_edits=inact,
 detour_hook_regions=[dict(rva='0x%x'%s,old=o.hex(),new=n.hex()) for s,o,n in sorted(detour)],
 detour_targets=['0x%x'%t for t in att],
 runtime_only=[dict(what='script-engine extension table pointer at exe RVA 0x7534a8 (BSS): heap pointer to the 114-entry {fn,argc} syscall table; installed by the 0x1f9a80 detour',see='syscall_table.json (S5) and part C')],
)
json.dump(out,open(HERE+'/edit_list.json','w'),indent=1)
# csv
import csv
with open(HERE+'/edit_list.csv','w',newline='') as f:
    w=csv.writer(f); w.writerow(['rva','section','file_offset','len','old','new','class','group','source_option','ini_value','effect_short'])
    for e in edits:
        so=';'.join(i['key'] for i in e['source_ini']) or '(unconditional)'
        iv=';'.join(str(i['shipped']) for i in e['source_ini'])
        w.writerow([e['rva'],e['section'],e['file_offset'],e['length'],e['old'],e['new'] or ('<disp32 -> .nmc+%s>'%e['buffer_offset']),e['cls'],e['group'],so,iv,e['effect'][:160]])
print(json.dumps(out['summary']))
