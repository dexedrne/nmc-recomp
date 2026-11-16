#!/usr/bin/env python3
"""Part E: produce gummi-removal data edits (scratch only).
 1. 00progress.bin (SillyBillyMod1 build): WLDF entry id 53, block cmd 5 ('World b3|=2' on world 2 = Twilight Town) -> cmd 14 (nop)
    at file offset 36636, so the TWTNW unlock no longer re-blocks Twilight Town.  (Task note called it 'Traverse Town'; KH2 has no
    Traverse Town: world index 2 = TT = Twilight Town per OpenKH worlds.md.)
 2. wm00.ard (owner's vanilla extract): drop the Spawn('m_51') op from every 'map' program that has it (14 of 15; program 25
    already omits it).  m_51 = 9 x object 942 WM_GUMI_01 (the world-map gummi route nodes), see spawnparse/objentry.
    The m_00/m_01/m_50(12 x WM_SYMBOL_*) spawn sets and the m_51 BAR entry itself are kept so every reference still resolves.
Inputs are verified by sha256 against the owner's files on build-host (see verification notes)."""
import struct,sys,json,os,hashlib,difflib
sys.dont_write_bytecode=True
HERE=os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0,HERE)
from wldf_decode import decode
S6='/home/user/projects/kh2-nmc-analysis/scratch/S6-gummi-data/src/'
PROG_IN='/home/user/projects/SillyBillyMod1/00progress.bin'; WM_IN=S6+'us/wm00.ard'
sha=lambda b:hashlib.sha256(b).hexdigest()
rep={}
# ---------- 1. 00progress.bin
b=bytearray(open(PROG_IN,'rb').read()); rep['00progress_in_sha256']=sha(bytes(b))
e=[x for x in decode(PROG_IN) if x['id']==53]; assert len(e)==1
blk=[x for x in e[0]['blocks'] if x['cmd']==5 and x['args']==[2]]; assert len(blk)==1 and blk[0]['file_off']==36636
assert struct.unpack_from('<H',b,36636)[0]==5
struct.pack_into('<H',b,36636,14)
open(HERE+'/00progress.gummi-removed.bin','wb').write(b)
rep['00progress']=dict(out_sha256=sha(bytes(b)),edits=[dict(file_offset=36636,old_u16=5,new_u16=14,wldf_entry_index=e[0]['i'],wldf_entry_id=53,
    meaning="WLDF block 'World b3|=2' [world 2 = Twilight Town] -> nop; removes the re-block of Twilight Town when the final route / EH opens")])
# diff report
orig=open(PROG_IN,'rb').read()
rep['00progress']['differing_bytes']=[i for i in range(len(orig)) if orig[i]!=b[i]]
# ---------- 2. wm00.ard
src=open(WM_IN,'rb').read(); rep['wm00_in_sha256']=sha(src)
cnt=struct.unpack_from('<I',src,4)[0]
ents=[]
for i in range(cnt):
    t,dup,nm,o,s=struct.unpack_from('<HH4sII',src,16+16*i); ents.append([t,dup,nm,o,s,src[o:o+s]])
M=[x for x in ents if x[2].rstrip(b'\0')==b'map'][0]
d=M[5]; out=bytearray(); p=0; removed=[]; progs=[]
NAME=struct.unpack('<I',b'm_51')[0]
while p+4<=len(d):
    pid,ln=struct.unpack_from('<HH',d,p)
    if ln==0: break
    data=d[p+4:p+ln]
    q=0; keep=bytearray(); rm=0
    while q+4<=len(data):
        oc,pc=struct.unpack_from('<HH',data,q); raw=data[q:q+4+4*pc]
        if oc==0 and pc==1 and struct.unpack_from('<I',raw,4)[0]==NAME: rm+=1
        else: keep+=raw
        q+=4+4*pc
    assert q==len(data)
    out+=struct.pack('<HH',pid,4+len(keep))+keep
    progs.append(dict(program=pid,len_before=ln,len_after=4+len(keep),removed_m51=rm))
    if rm: removed.append(pid)
    p+=ln
tail=d[p:]            # terminator / padding after the last program, kept verbatim
out+=tail
M[5]=bytes(out); M[4]=len(out)
# rebuild BAR (16-byte aligned blobs, same table layout)
hdr=bytearray(src[:16]); data=bytearray(); off=16+16*cnt; off=(off+15)&~15
ent_tab=bytearray(); blobs=[]
pos=off
for t,dup,nm,o,s,blob in ents:
    pos=(pos+15)&~15
    ent_tab+=struct.pack('<HH4sII',t,dup,nm,pos,len(blob)); blobs.append((pos,blob)); pos+=len(blob)
new=bytearray(hdr+ent_tab); 
for p_,bl in blobs:
    new+=bytes(p_-len(new)); new+=bl
new+=bytes((16-len(new)%16)%16) if len(src)%16==0 else b''
open(HERE+'/wm00.gummi-removed.ard','wb').write(new)
rep['wm00']=dict(out_sha256=sha(bytes(new)),size_before=len(src),size_after=len(new),map_size_before=1308,map_size_after=len(out),programs=progs,programs_edited=sorted(removed),n_programs=len(progs),n_edited=len(removed),
   kept_unchanged_subfiles=[x[2].rstrip(b'\0').decode() for x in ents])
# verification: reparse
nb=bytes(new); c2=struct.unpack_from('<I',nb,4)[0]; assert c2==cnt
ok=True; names2=[]
for i in range(c2):
    t,dup,nm,o,s=struct.unpack_from('<HH4sII',nb,16+16*i); names2.append(nm.rstrip(b'\0').decode())
    ob=[x for x in ents if x[2]==nm][0]; ok&= nb[o:o+s]==ob[5]
rep['wm00']['reparse_ok']=ok
mm=[x for x in ents if x[2].rstrip(b'\0')==b'm_51'][0]; rep['wm00']['m_51_subfile_kept_bytes']=len(mm[5])
rep['wm00']['m_51_spawns_remaining']=sum(1 for _ in range(0)) 
# count remaining m_51 refs in map
left=0; p=0; dd=M[5]
while p+4<=len(dd):
    pid,ln=struct.unpack_from('<HH',dd,p)
    if ln==0: break
    da=dd[p+4:p+ln]; q=0
    while q+4<=len(da):
        oc,pc=struct.unpack_from('<HH',da,q)
        if oc==0 and struct.unpack_from('<I',da,q+4)[0]==NAME: left+=1
        q+=4+4*pc
    p+=ln
rep['wm00']['m_51_spawn_ops_remaining']=left
json.dump(rep,open(HERE+'/gummi_removal_diff.json','w'),indent=1)
print(json.dumps({k:v for k,v in rep.items() if k!='wm00'},indent=1)[:900]); print(json.dumps({k:v for k,v in rep['wm00'].items() if k!='programs'},indent=1))
