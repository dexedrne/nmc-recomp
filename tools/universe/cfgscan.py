"""Recursive-descent CFG scan with MSVC in-text jump-table recovery. Pure static decoding (capstone)."""
import struct, bisect
from capstone import CS_OP_IMM, CS_OP_MEM, CS_OP_REG
import capstone.x86_const as X
import x64scan as S
from x64scan import md, img, BASE, in_exec, JCC, RIP
PADMN={'int3'}
def is_nop(i):
    return i.mnemonic in('nop','fnop') or (i.mnemonic=='xchg' and len(i.operands)==2 and i.operands[0].type==CS_OP_REG and i.operands[1].type==CS_OP_REG and i.operands[0].reg==i.operands[1].reg and i.size==2) or (i.mnemonic=='lea' and False)
def resolve_jt(rec, jmp, ok=None):
    """rec: previous instructions (address order, nearest last). Returns (jt|None, suspect)."""
    reg_base={}; bounds=[]; tab=None; btab=None; seen_idx=False
    for i in rec:
        m=i.mnemonic
        if m=='lea' and i.operands[1].type==CS_OP_MEM and i.operands[1].mem.base==RIP:
            reg_base[i.operands[0].reg]=i.address+i.size+i.operands[1].mem.disp-BASE
        elif m=='cmp' and len(i.operands)==2 and i.operands[1].type==CS_OP_IMM and i.operands[1].imm>=0:
            bounds.append(i.operands[1].imm+1)
        elif m=='and' and len(i.operands)==2 and i.operands[1].type==CS_OP_IMM and 0<=i.operands[1].imm<=0xffff:
            bounds.append(i.operands[1].imm+1)
        elif m in('mov','movsxd','movzx') and len(i.operands)==2 and i.operands[1].type==CS_OP_MEM:
            mm=i.operands[1].mem
            if mm.index and mm.base and mm.base!=RIP:
                seen_idx=True
                if mm.scale==4 and m in('mov','movsxd'): tab=(mm.base,mm.disp)
                elif mm.scale==1 and m=='movzx' and i.operands[1].size==1 and mm.disp>=0x1000: btab=(mm.base,mm.disp)
    if tab is None: return None,seen_idx
    bb,disp=tab
    assumed=False
    if bb in reg_base: rel=reg_base[bb]
    else:
        rel=0; assumed=True      # base register holds __ImageBase (set earlier in the function); validated by target check
        if not (0x1000<=disp<len(img)): return None,True
    taddr=rel+disp
    def valid(t): return in_exec(t) and (ok is None or ok(t))
    bt_addr=None
    if btab is not None: bt_addr=(reg_base[btab[0]] if btab[0] in reg_base else 0)+btab[1]
    def mk(n,how,**kw):
        tg=[rel+struct.unpack_from('<i',img,taddr+4*k)[0] for k in range(n)]
        return dict(site=jmp.address-BASE,table=taddr,n=n,base=rel,targets=tg,byte_table=(bt_addr,kw.get('btn')),assumed_base=assumed,bound=how)
    def check(n):
        if n<=0 or n>4096 or taddr+4*n>len(img): return False
        return all(valid(rel+struct.unpack_from('<i',img,taddr+4*k)[0]) for k in range(n))
    cand=list(reversed(bounds))
    if bt_addr is not None:
        for bn in cand:
            if bn>0x10000 or bt_addr+bn>len(img): continue
            n=max(img[bt_addr:bt_addr+bn])+1
            if check(n): return mk(n,'cmp/and+byte-table',btn=bn),True
        # table directly followed by its byte table: n limited by it
        if bt_addr>taddr:
            n=0
            while n<(bt_addr-taddr)//4 and valid(rel+struct.unpack_from('<i',img,taddr+4*n)[0]): n+=1
            if n>=2:
                d=mk(n,'scanned-to-byte-table',btn=(cand[0] if cand and cand[0]<=0x10000 else None)); return d,True
        return None,True
    for bn in cand:
        if check(bn): return mk(bn,'cmp/and'),True
    # register-compare bound: scan until first invalid
    n=0
    while n<4096 and taddr+4*n+4<=len(img) and valid(rel+struct.unpack_from('<i',img,taddr+4*n)[0]): n+=1
    if n>=2: return mk(n,'scanned'),True
    return None,True
def analyze(seeds, allowed, stops=frozenset(), own_entries=frozenset(), fstart=None, forbid=None):
    """seeds: rva list. allowed(r)->bool (is rva within the permitted code ranges).
    stops: rvas of OTHER known function starts: flow reaching one stops (tail/fallthrough).
    Returns dict."""
    insn={}      # addr -> size
    by_end={}    # end -> insn object (for look-back); store minimal (object kept)
    calls=[]; jmp_out=[]; jmp_in=[]; rip_code=[]; icall_mem=[]; ijmp_mem=[]; icall_reg=0
    ijmp_nonrip=[]; fall_out=[]; fall_stop=[]; jtabs=[]; unres=[]; susp=[]; misalign=[]; bad_seeds=[]
    pend=[]      # pending indirect register jumps (insn)
    nterm=0
    work=list(seeds)
    objs={}
    def run():
        nonlocal icall_reg,nterm
        while work:
            a=work.pop()
            while True:
                if a in insn: break
                if not allowed(a): bad_seeds.append(a); break
                top=min(a+512,len(img))
                nxt=None; stopped=False
                for i in md.disasm(img[a:top],BASE+a,0):
                    ia=i.address-BASE
                    if ia in insn: stopped=True; break
                    if ia!=a and ia in stops and ia not in own_entries: fall_stop.append(ia); stopped=True; break
                    if not allowed(ia) or not allowed(ia+i.size-1):
                        fall_out.append(ia); stopped=True; break
                    if forbid and (ia in forbid or (ia+i.size-1) in forbid):
                        fall_stop.append(ia); stopped=True; break
                    insn[ia]=i.size; objs[ia]=i; by_end[ia+i.size]=ia
                    m=i.mnemonic; ops=i.operands
                    for o in ops:
                        if o.type==CS_OP_MEM and o.mem.base==RIP and m not in('call','jmp'):
                            t=ia+i.size+o.mem.disp
                            if in_exec(t): rip_code.append((ia,t,m))
                            break
                    nxt=ia+i.size
                    if m=='call':
                        o=ops[0]
                        if o.type==CS_OP_IMM: calls.append((ia,o.imm-BASE))
                        elif o.type==CS_OP_MEM and o.mem.base==RIP: icall_mem.append((ia,ia+i.size+o.mem.disp))
                        else: icall_reg+=1
                        continue
                    if m=='jmp' or m in JCC:
                        o=ops[0]
                        if o.type==CS_OP_IMM:
                            t=o.imm-BASE
                            if allowed(t) and not (t in stops and t not in own_entries) and not (fstart and fstart(t)):
                                jmp_in.append((ia,t,m!='jmp')); work.append(t)
                            else: jmp_out.append((ia,t,m!='jmp'))
                        elif m=='jmp':
                            if o.type==CS_OP_MEM and o.mem.base==RIP: ijmp_mem.append((ia,ia+i.size+o.mem.disp))
                            elif o.type==CS_OP_MEM: ijmp_nonrip.append(ia)
                            else: pend.append(ia)
                        if m=='jmp': nterm+=1; stopped=True; break
                        continue
                    if m in('ret','retf','iret','iretq','ud2','int3','hlt'):
                        nterm+=1; stopped=True; break
                if stopped or nxt is None: break
                a=nxt
    run()
    # jump-table resolution fixpoint
    resolved=set()
    while True:
        progress=False
        for ja in list(pend):
            if ja in resolved: continue
            # look-back by address chain
            rec=[]; e=ja
            while len(rec)<24 and e in by_end:
                s=by_end[e]; rec.append(objs[s]); e=s
            rec.reverse()
            jt,sus=resolve_jt(rec,objs[ja],allowed)
            if jt:
                resolved.add(ja); jtabs.append(jt); progress=True
                for t in jt['targets']:
                    if allowed(t) and t not in (stops-own_entries): work.append(t)
                    else: jmp_out.append((ja,t,False))
        if progress: run()
        else: break
    for ja in pend:
        if ja in resolved: continue
        rec=[]; e=ja
        while len(rec)<24 and e in by_end:
            s=by_end[e]; rec.append(objs[s]); e=s
        rec.reverse()
        (susp if any(j.mnemonic in('mov','movsxd') and j.operands[1].type==CS_OP_MEM and j.operands[1].mem.index and j.operands[1].mem.scale==4 for j in rec) else unres).append(ja)
    return dict(insn=insn,calls=calls,jmp_in=jmp_in,jmp_out=jmp_out,rip_code=rip_code,icall_mem=icall_mem,ijmp_mem=ijmp_mem,icall_reg=icall_reg,ijmp_nonrip=ijmp_nonrip,
                jtabs=jtabs,unres_ijmp=unres,susp_jt=susp,fall_out=fall_out,fall_stop=fall_stop,bad_seeds=bad_seeds)


def analyze2(seeds, allowed, stops=frozenset(), own_entries=frozenset(), fstart=None):
    """analyze, then re-run with jump-table bytes forbidden so fallthrough into table data is never decoded as code"""
    r=analyze(seeds,allowed,stops,own_entries,fstart)
    if r['jtabs']:
        fb=set()
        for jt in r['jtabs']:
            fb.update(range(jt['table'],jt['table']+4*jt['n']))
            bt=jt['byte_table']
            if bt[0] is not None and bt[1]: fb.update(range(bt[0],bt[0]+bt[1]))
        r=analyze(seeds,allowed,stops,own_entries,fstart,forbid=fb)
    return r
