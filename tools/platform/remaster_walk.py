"""Faithful Python port of Panacea's KH2 container walkers (GetIMDOffsets ... GetBAROffsets) and the
ScanRemasteredFolder selection logic, used to check a loose-tree 'remastered' layout against real data.
Written from reading OpenKh.Research.Panacea/Panacea.cpp (Apache-2.0). Pure functions on bytes."""
import struct, os, re, sys
def i32(b,o): return struct.unpack_from('<i',b,o)[0]
def u32(b,o): return struct.unpack_from('<I',b,o)[0]
def i16(b,o): return struct.unpack_from('<h',b,o)[0]
def i64(b,o): return struct.unpack_from('<q',b,o)[0]
M32=0xffffffff
def s32(v): 
    v&=M32
    return v-(1<<32) if v&0x80000000 else v
def get_imd(b,a,base,ent):
    if a+16<=len(b) and b[a:a+4]==b'IMGD':
        ent.append(s32(i32(b,a+8)+base+0x20000000))
def get_imz(b,a,base,ent):
    if b[a:a+4]==b'IMGZ':
        cnt=i32(b,a+12); off=a+16
        while cnt>0:
            cnt-=1
            imdoff=i32(b,off)
            get_imd(b,a+imdoff,base+imdoff,ent)
            off+=8
def get_tm2(b,a,base,ent):
    if b[a:a+4]==b'TIM2':
        cnt=i16(b,a+6); off=a+0x10; base+=0x10
        while cnt>0:
            cnt-=1
            nxt=i32(b,off)
            ent.append(s32(i16(b,off+12)+base+0x20000000))
            off+=nxt; base+=nxt
def get_dpd(b,a,base,ent):
    p=a
    if i32(b,p)!=0x96: return
    p+=4
    p+=(i32(b,p)+1)*4
    texcnt=i32(b,p); p+=4
    tex=[]
    for t in range(texcnt):
        texoff=i32(b,p); p+=4
        tp=a+texoff
        tex.append(dict(texoff=texoff,TBP0=i16(b,tp),shX=i16(b,tp+8),shY=i16(b,tp+10),unique=False))
    for i in range(len(tex)):
        for j in range(i+1,len(tex)):
            if tex[i]['TBP0']==tex[j]['TBP0'] and tex[i]['shX']==tex[j]['shX'] and tex[i]['shY']==tex[j]['shY']:
                tex[i]['unique']=True; tex[j]['unique']=True
    tex.sort(key=lambda t:t['TBP0'])   # std::sort (not stable); equal TBP0 order is unspecified upstream
    uniq={t['TBP0'] for t in tex if t['unique']}
    for t in tex:
        if t['TBP0'] in uniq: t['unique']=True
    shared={}
    for t in tex:
        if not t['unique']:
            if t['TBP0'] in shared:
                offset=shared[t['TBP0']]
                ent[-1]=s32(offset+base+0x20000000)
                continue
            else:
                offset=t['texoff']+0x20
                shared[t['TBP0']]=offset
        else:
            offset=t['texoff']+0x20
        ent.append(s32(offset+base+0x20000000))
def get_dpx(b,a,base,ent):
    p=a+i32(b,a)*0x20+4
    cnt=i32(b,p); p+=4
    while cnt>0:
        cnt-=1
        dpdoff=i32(b,p)-0xC; p+=4
        get_dpd(b,a+dpdoff,base+dpdoff,ent)
def get_pax(b,a,base,ent):
    if b[a:a+4]==b'PAX_':
        dpxoff=i32(b,a+12)
        get_dpx(b,a+dpxoff+12,base+dpxoff+12,ent)
def get_raw(b,a,end,base,ent):
    if i32(b,a)==0:
        tic=i32(b,a+8); gsc=i32(b,a+12); offdata=i32(b,a+16); clut=i32(b,a+20); gsinfo=i32(b,a+24)
        pic=[i32(b,a+clut+0x90+t*0x90+116) for t in range(tic)]
        mod=0
        for m in range(gsc):
            tex0=i64(b,a+gsinfo+0x70+m*0xA0); psm=(tex0>>20)&0x3f
            if psm!=20: mod+=1
        p4=p8=0; basetex=[]
        for p in range(gsc):
            key=b[a+offdata+p]; key=key-256 if key>127 else key   # char
            tex0=i64(b,a+gsinfo+0x70+p*0xA0); psm=(tex0>>20)&0x3f
            if key<0 or key>=tic:   # Panacea reads PicOffsets[key] out of its array here (signed char index): never-matching offset, as runtime/remaster.c
                fo=0x3fffffff
                if psm==20: p4+=1
                else: p8+=1
            else:
                fo=s32(base+pic[key]+0x20000000)
                if psm==20: fo+=p4+mod*0x10; p4+=1
                else: fo+=p8*0x10; p8+=1
            basetex.append(s32(fo))
        texa={}
        pos=b.find(b'TEXA',a,end)
        while pos!=-1 and pos<end:
            img=i16(b,pos+0x0A); to=i32(b,pos+0x28)
            texa.setdefault(img,[]).append(s32(base+(pos-a)+to+8+img*0x10+0x20000000))
            pos=b.find(b'TEXA',pos+4,end)
        for i,v in enumerate(basetex):
            ent.append(v)
            for t in texa.get(i,[]): ent.append(t)
def get_bar(b,a,base,ent):
    if b[a:a+4]==b'BAR\x01':
        tim=[];pax=[];tm2=[];gen=[];aud=[]
        cnt=i32(b,a+4); off=a+16
        while cnt>0:
            cnt-=1
            typ=i32(b,off); datoff=i32(b,off+8); sz=i32(b,off+12)
            if typ==7: get_raw(b,a+datoff,a+datoff+sz,base+datoff,tim)
            elif typ==10: get_tm2(b,a+datoff,base+datoff,tm2)
            elif typ==18: get_pax(b,a+datoff,base+datoff,pax)
            elif typ==24: get_imd(b,a+datoff,base+datoff,gen)
            elif typ==29: get_imz(b,a+datoff,base+datoff,gen)
            elif typ in(31,34):
                if b[a+datoff:a+datoff+6]==b'ORIGIN': aud.append(-1)
            elif typ==36: gen.append(s32(base+datoff+0x20000000))
            elif typ==46: get_bar(b,a+datoff,base+datoff,gen)
            off+=16
        ent+=tim+pax+tm2+gen+aud
BAR_EXTS={'.2dd','.2ld','.a.fm','.a.fr','.a.gr','.a.it','.a.sp','.a.us','.a.uk','.a.jp','.bar','.bin','.mag','.map','.mdlx'}
def kh2_ext(path):
    base=os.path.basename(path); i=base.rfind('.')
    if i<0: return ''
    ext=base[i:].lower()
    if i>=2 and base[i-2]=='.' and base[i-1].lower()=='a': ext='.a'+ext
    return ext
def asset_offsets(path,b):
    ext=kh2_ext(path); ent=[]
    if ext=='.imd': get_imd(b,0,0,ent)
    elif ext=='.imz': get_imz(b,0,0,ent)
    elif ext=='.tm2': get_tm2(b,0,0,ent)
    elif ext=='.pax': get_pax(b,0,0,ent)
    elif ext=='.dpd': get_dpd(b,0,0,ent)
    elif ext in BAR_EXTS: get_bar(b,0,0,ent)
    else: return None   # unknown type: upstream falls back to the package's own entry origOffsets
    return ent
