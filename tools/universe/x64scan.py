"""Instruction-level scanning helpers (capstone, x64). No execution of any code from the exe."""
import struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_OP_IMM, CS_OP_MEM, CS_OP_REG
import capstone.x86_const as X
from pecore import PEImg
P=PEImg(); img=P.img; BASE=P.base
EXEC=[(a,b) for _,a,b in P.exec_secs]
def in_exec(r): return any(a<=r<b for a,b in EXEC)
IATRANGE=(P.dd['IAT'][0],P.dd['IAT'][0]+P.dd['IAT'][1])
md=Cs(CS_ARCH_X86,CS_MODE_64); md.detail=True
RIP=X.X86_REG_RIP
JCC={'ja','jae','jb','jbe','je','jne','jg','jge','jl','jle','jo','jno','jp','jnp','js','jns','jcxz','jecxz','jrcxz','loop','loope','loopne'}
def decode_one(a):
    for i in md.disasm(img[a:a+15],a,1): return i
    return None
class Scan:
    """accumulates refs found in instruction streams"""
    def __init__(self):
        self.calls=[];self.jmps=[];self.rip_code=[];self.icall_mem=[];self.ijmp_mem=[];self.icall_reg=0
        self.jtabs=[];self.unres_ijmp=[];self.ninsn=0;self.invalid=0
        self.recent=[]
    def feed(self,i):
        """process instruction i; returns 'ret'|'jmp'|'ud'|None (terminal kind for linear flow)"""
        self.ninsn+=1
        m=i.mnemonic; ops=i.operands
        self.recent.append(i)
        if len(self.recent)>24: del self.recent[0]
        term=None
        if m=='call':
            o=ops[0]
            if o.type==CS_OP_IMM: self.calls.append((i.address-BASE,o.imm-BASE))
            elif o.type==CS_OP_MEM and o.mem.base==RIP: self.icall_mem.append((i.address-BASE,i.address+i.size+o.mem.disp-BASE))
            else: self.icall_reg+=1
        elif m=='jmp':
            o=ops[0]; term='jmp'
            if o.type==CS_OP_IMM: self.jmps.append((i.address-BASE,o.imm-BASE,False))
            elif o.type==CS_OP_MEM and o.mem.base==RIP: self.ijmp_mem.append((i.address-BASE,i.address+i.size+o.mem.disp-BASE))
            else:
                jt=self.resolve_jt(i)
                if jt: self.jtabs.append(jt)
                else: self.unres_ijmp.append(i.address-BASE)
        elif m in JCC:
            o=ops[0]
            if o.type==CS_OP_IMM: self.jmps.append((i.address-BASE,o.imm-BASE,True))
        elif m in('ret','retf','iret','iretq'): term='ret'
        elif m=='ud2': term='ud'
        # rip-relative address formation into code
        for o in ops:
            if o.type==CS_OP_MEM and o.mem.base==RIP and m not in('call','jmp'):
                t=i.address+i.size+o.mem.disp-BASE
                if in_exec(t): self.rip_code.append((i.address-BASE,t,m))
                break
        return term
    def resolve_jt(self,jmp):
        """MSVC x64 jump table: [cmp idx,N; ja d;] lea base; [movzx b,[base+idx+T2];] mov e,[base+idx*4+T]; add; jmp reg"""
        rec=self.recent[:-1]
        reg_base={}   # reg -> rva value
        bound=None; tab=None; btab=None; tabbase=None
        for i in rec:
            m=i.mnemonic
            if m=='lea' and i.operands[1].type==CS_OP_MEM and i.operands[1].mem.base==RIP:
                reg_base[i.operands[0].reg]=i.address+i.size+i.operands[1].mem.disp-BASE
            elif m=='cmp' and len(i.operands)==2 and i.operands[1].type==CS_OP_IMM and i.operands[1].imm>=0:
                bound=i.operands[1].imm+1
            elif m in('mov','movsxd','movzx') and len(i.operands)==2 and i.operands[1].type==CS_OP_MEM:
                mm=i.operands[1].mem
                if mm.index and mm.base and mm.base!=RIP:
                    if mm.scale==4 and m in('mov','movsxd'):
                        tab=(mm.base,mm.disp); 
                    elif mm.scale==1 and m=='movzx':
                        btab=(mm.base,mm.disp)
        if tab is None: return None
        bb,disp=tab
        # entries are RVAs relative to image base (lea of __ImageBase gives rva 0) or relative to a lea'd table
        if bb in reg_base:
            bval=reg_base[bb]
            taddr=bval+disp; rel=bval
        else:
            return None
        n=bound if bound else None
        if btab is not None and btab[0] in reg_base:
            bt=reg_base[btab[0]]+btab[1]
            if n is None or n>0x10000: return None
            idxs=img[bt:bt+n]; n=max(idxs)+1
        if n is None or n<=0 or n>4096: return None
        if not (0<=taddr<len(img)-4*n): return None
        tg=[]
        for k in range(n):
            v=struct.unpack_from('<i',img,taddr+4*k)[0]
            t=rel+v
            if not in_exec(t): return None
            tg.append(t)
        return dict(site=jmp.address-BASE,table=taddr,n=n,base=rel,targets=tg,bytetable=(btab is not None))
def linear(a,b,sc=None):
    """linear sweep of rva range [a,b). returns (Scan, last_terminal_kind, bad_offsets). last insn end == b iff clean."""
    sc=sc or Scan()
    p=a; term=None; bad=[]
    while p<b:
        q=p
        for i in md.disasm(img[p:b],BASE+p,0):
            term=sc.feed(i); q=i.address-BASE+i.size
        if q>=b: break
        if q==p:
            bad.append(p); sc.invalid+=1; p+=1; term=None
        else: p=q
    return sc,term,bad
