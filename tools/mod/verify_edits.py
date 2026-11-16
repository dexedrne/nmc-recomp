#!/usr/bin/env python3
"""Verify kh2fm.nmc.exe against the pristine exe and the mod's own signature tables. Writes verification.json."""
import json, struct, sys, os, re, collections
sys.dont_write_bytecode=True
HERE=os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0,HERE)
from pelib import *
import capstone
S5='/home/user/projects/kh2-nmc-analysis/scratch/S5-mod-hooks/'
E=json.load(open(HERE+'/edit_list.json')); AP=json.load(open(HERE+'/applied.json'))
pri=PE(); pat=PE(HERE+'/kh2fm.nmc.exe')
R={}
# 1. file-level diff
a,b=pri.raw,pat.raw
diff=[i for i in range(len(a)) if a[i]!=b[i]]
exp=set()
for e in AP['applied']:
    fo=pri.file_off(int(e['rva'],16)); old=bytes.fromhex(e['old'])
    for k in range(len(old)):
        if old[k]!=bytes.fromhex(e['new'])[k]: exp.add(fo+k)
hdr=set(i for i in diff if i<0x400)
body=[i for i in diff if 0x400<=i<len(a)]
R['file_diff']=dict(pristine_size=len(a),patched_size=len(b),appended_bytes=len(b)-len(a),
   differing_bytes_in_original_extent=len(diff),differing_in_headers=sorted(hdr)[:3] and len(hdr),
   body_diff_equals_edit_bytes=(set(body)==exp),edit_byte_count=len(exp))
assert set(body)==exp and len(body)==len(exp)
# the edit list "old->new" bytes net change (a region can contain unchanged bytes inside it)
# 2. mapped-image diff (RVA space) below the new section
n=min(len(pri.img),len(pat.img)); mdiff=[i for i in range(len(pri.img)) if pri.img[i]!=pat.img[i]]
medits=set()
for e in AP['applied']:
    r=int(e['rva'],16); o=bytes.fromhex(e['old']); nn=bytes.fromhex(e['new'])
    medits|={r+k for k in range(len(o)) if o[k]!=nn[k]}
R['mapped_diff']=dict(pristine_image_size=len(pri.img),patched_image_size=len(pat.img),differing_rvas_below_old_SizeOfImage=len([i for i in mdiff if 0x400<=i<len(pri.img)]),equals_edit_bytes=(set(i for i in mdiff if 0x400<=i<len(pri.img))==medits))
hdrd=[i for i in mdiff if i<0x400]; R['mapped_diff']['header_bytes_differing']=len(hdrd)
assert set(i for i in mdiff if 0x400<=i<len(pri.img))==medits
# 3. every edit old==pristine and new==patched; rel32 targets land in .nmc and the targeted bytes equal the DLL buffer
nmc_rva=int(AP['nmc_section']['rva'],16)
bad=[]
for e in AP['applied']:
    r=int(e['rva'],16); o=bytes.fromhex(e['old']); nn=bytes.fromhex(e['new'])
    if pri.img[r:r+len(o)]!=o or pat.img[r:r+len(nn)]!=nn: bad.append(e['rva'])
R['per_edit_old_new_check']=dict(edits=len(AP['applied']),mismatches=bad)
assert not bad
rel=[e for e in AP['applied'] if e['cls']=='rel32_to_new_section']
rel_ok=[]
for e in rel:
    r=int(e['rva'],16); disp=struct.unpack('<i',bytes.fromhex(e['new']))[0]; t=r+4+disp
    rel_ok.append(dict(site=e['rva'],target_rva='0x%x'%t,nmc_offset='0x%x'%(t-nmc_rva),in_nmc=nmc_rva<=t<nmc_rva+0x1000))
R['rel32_targets']=rel_ok
assert all(x['in_nmc'] for x in rel_ok)
# float check of the relocated blow gravity
bgrva=nmc_rva; R['blow_gravity_float']=struct.unpack('<f',pat.img[bgrva:bgrva+4])[0]
# 4. cross-check with the DLL-emulation image: apply the static diff regions of the in-process run to the pristine image and compare
import pickle
d=pickle.load(open(HERE+'/emu_runs/wp54.pkl','rb')); att=[t for t,h in d['attach']]
emu=bytearray(pri.img); 
for s,o,nn in d['diff']:
    if any(t<=s<t+16 for t in att): continue
    emu[s:s+len(nn)]=nn
# neutralise the 8 relocated disp32 and 2 bss writes before comparing
cmp_bad=[]
reloc_sites={int(e['rva'],16) for e in rel}|{0x7534a8,0x7534ac}
for s,o,nn in d['diff']:
    if any(t<=s<t+16 for t in att) or s in reloc_sites: continue
    if pat.img[s:s+len(nn)]!=nn: cmp_bad.append(hex(s))
R['emulated_inprocess_diff_vs_patched_image']=dict(compared_regions=89-len(reloc_sites),mismatches=cmp_bad,relocated_sites=len(rel),bss_only_regions=2)
assert not cmp_bad
# the .nmc content equals the DLL's allocated buffer
bufemu=d['extra'][0x13fff0000]
fo=pat.file_off(nmc_rva); R['nmc_content_equals_dll_buffer']=(bytes(pat.raw[fo:fo+0x200])==bufemu[:0x200] and not any(bufemu[0x200:]))
assert R['nmc_content_equals_dll_buffer']
# 5. detour hook sites are untouched (the 35 hooks are source-level replacements, not byte edits)
unt=[t for t in d['attach']]
R['detour_sites_untouched_in_patched_image']=all(pri.img[t:t+16]==pat.img[t:t+16] for t,h in d['attach'])
# 6. AntiForm sites untouched
R['antiform_sites_untouched']=all(pri.img[int(x['rva'],16):int(x['rva'],16)+x['length']]==pat.img[int(x['rva'],16):int(x['rva'],16)+x['length']] for x in E['inactive_edits'])
# 7. signature tables
sigs=json.load(open(S5+'signatures_refactor.json')); nfly=json.load(open(S5+'signatures_neverlandfly.json')); moose=json.load(open(S5+'moose_pats.json'))
def hits(img,p):
    return [m.start() for m in pat_to_re(p).finditer(img,0,0x2c2b000)] if False else [m.start() for m in re.compile(b''.join(b'.' if t=='??' else re.escape(bytes([int(t,16)])) for t in p.split()),re.S).finditer(img[:0x2bd7000])]
import time
t=time.time()
sres=[]
mism=[]
img_p=pri.img; img_q=pat.img
for sg in sigs:
    p=sg['pattern']
    hp=hits(img_p,p); hq=hits(img_q,p)
    first=int(sg['first_hit_rva'],16) if sg['first_hit_rva'] else None
    res=int(sg['resolved_rva'],16) if sg['resolved_rva'] else None
    adj=None
    if first is not None and res is not None and sg['resolution'].startswith('direct'): adj=res-first
    r=dict(scanner_rva=sg['scanner_rva'],pattern_len=sg['pattern_len'],hits_pristine=len(hp),hits_patched=len(hq),
       recorded_hits=sg['hits_in_exe'],first_hit_pristine=hex(hp[0]) if hp else None,first_hit_patched=hex(hq[0]) if hq else None,
       recorded_first_hit=sg['first_hit_rva'],resolution=sg['resolution'],resolved_rva=sg['resolved_rva'])
    # recompute the resolved RVA on both images
    def resolve(img,hs):
        if not hs: return None
        f=hs[0]
        if sg['resolution'].startswith('direct'): return f+adj
        m=re.match(r'follow_call_at\(\+(\d+)\)',sg['resolution'])
        if m:
            k=int(m.group(1)); 
            if img[f+k] in (0xe8,0xe9): return (f+k+5+struct.unpack_from('<i',img,f+k+1)[0])&0xffffffff
        return None
    r['resolved_pristine']=resolve(img_p,hp); r['resolved_patched']=resolve(img_q,hq)
    r['ok_pristine']=(len(hp)==sg['hits_in_exe'] and (not hp or hex(hp[0])==sg['first_hit_rva']) and (res is None or r['resolved_pristine'] in (None,res)))
    r['same_after_patch']=(hp==hq)
    sres.append(r)
R['signature_scan']=dict(table='S5 signatures_refactor.json',count=len(sres),
   pristine_match_recorded=sum(1 for r in sres if r['ok_pristine']),
   hits_identical_pristine_vs_patched=sum(1 for r in sres if r['same_after_patch']),
   resolved_identical=sum(1 for r in sres if r['resolved_pristine']==r['resolved_patched']),
   not_matching_recorded=[(r['scanner_rva'],r['hits_pristine'],r['recorded_hits'],r['resolution']) for r in sres if not r['ok_pristine']],
   changed_by_patch=[(r['scanner_rva'],r['hits_pristine'],r['hits_patched']) for r in sres if not r['same_after_patch']])
# NeverlandFly + Moose
nres=[]
for sg in nfly:
    hp=hits(img_p,sg['pattern']); hq=hits(img_q,sg['pattern'])
    nres.append(dict(name=sg['name'],hits_pristine=[hex(x) for x in hp],hits_patched=[hex(x) for x in hq],recorded=sg['hit_rvas'],ok=([hex(x) for x in hp]==sg['hit_rvas']),same_after_patch=hp==hq))
R['neverlandfly_signatures']=nres
mres=[]
for sg in moose:
    hp=hits(img_p,sg['pattern']); hq=hits(img_q,sg['pattern']); mres.append(dict(callback=sg['callback'],hits_pristine=[hex(x) for x in hp],hits_patched=[hex(x) for x in hq],recorded=sg['hits'],ok=[hex(x) for x in hp]==sg['hits'],same_after_patch=hp==hq))
R['moose_signatures']=mres
R['signature_scan_seconds']=round(time.time()-t,1)
# 8. which signature windows overlap the edit sites (a scanner run AFTER patching could fail) -- informational
ov=[]
for r,sg in zip(sres,sigs):
    if r['hits_pristine']>=1:
        h=int(r['first_hit_pristine'],16)
        for e in E['edits']:
            er=int(e['rva'],16)
            if h<=er<h+r['pattern_len'] or er<=h<er+e['length']: ov.append((r['scanner_rva'],e['rva']))
R['signature_windows_overlapping_static_edits']=ov
json.dump(R,open(HERE+'/verification.json','w'),indent=1)
print(json.dumps({k:v for k,v in R.items() if k not in ('rel32_targets','signature_scan')},indent=1)[:3500])
ss=R['signature_scan']; print({k:v for k,v in ss.items() if k!='table'})
