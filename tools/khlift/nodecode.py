import sys, collections
import pyvex, archinfo, capstone
from khlift import universe as U, BASE
ARCH=archinfo.ArchAMD64(); IMG=U.image(); FS=U.load()
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
res=[]
for f in FS:
    if f.nep: continue
    work=[f.lo]; done=set()
    while work:
        a=work.pop()
        if a in done or not f.lo<=a<f.hi: continue
        done.add(a)
        data=IMG[a-BASE:f.hi-BASE]
        irsb=pyvex.lift(data,a,ARCH,max_bytes=len(data),opt_level=1)
        for s in irsb.statements:
            if s.tag=='Ist_Exit' and f.lo<=s.dst.value<f.hi: work.append(s.dst.value)
        nx=irsb.next
        last=a; end=a
        for s in irsb.statements:
            if s.tag=='Ist_IMark': last=s.addr; end=s.addr+s.len
        if irsb.jumpkind in ('Ijk_NoDecode','Ijk_Yield','Ijk_Sys_int','Ijk_SigTRAP') :
            res.append((irsb.jumpkind,hex(f.rva),hex(end),[ (i.mnemonic,i.op_str) for i in md.disasm(IMG[end-BASE:end-BASE+15],end,1)]))
        if nx.tag=='Iex_Const' and f.lo<=nx.con.value<f.hi: work.append(nx.con.value)
        if irsb.jumpkind=='Ijk_Call': work.append(end)
        for t in f.jtabs.get(last,[]): work.append(t)
c=collections.Counter()
for r in res:
    if r[0]!='Ijk_SigTRAP': print(r)
    c[(r[0],tuple(r[3][0][:1]) if r[3] else None)]+=1
print(c.most_common(20))
