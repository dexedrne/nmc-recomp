"""Recursive descent over every logical (merged) .pdata function. Writes cfg_pdata.pkl and logical.pkl"""
import pickle, multiprocessing as mp, time, collections, bisect
import cfgscan as C, x64scan as S
exec(open('u2_eh.py').read().split("if __name__=='__main__':")[0])
# logical functions: root_idx -> list of entry idx
groups=collections.defaultdict(list)
for i,r in enumerate(out): groups[r['root_idx']].append(i)
LOG=[]
for ri,idxs in sorted(groups.items(), key=lambda kv: out[kv[0]]['b']):
    rg=sorted((out[i]['b'],out[i]['e']) for i in idxs)
    LOG.append(dict(root=out[ri]['b'],ranges=rg,idxs=sorted(idxs)))
ALLBEGINS=frozenset(r['b'] for r in out)
EXTRA={}
SEHJT=collections.defaultdict(list)
_eb={r['b']:i for i,r in enumerate(out)}
for eb,tab in seh_tables.items():
    rb=out[out[_eb[eb]]['root_idx']]['b']
    for bg,en,hn,jt in tab:
        if jt: SEHJT[rb].append(jt)
def work(chunk):
    res={}
    for L in chunk:
        rg=L['ranges']; starts_=[a for a,b in rg]
        def allowed(x,rg=rg,st=starts_):
            k=bisect.bisect_right(st,x)-1
            return k>=0 and x<rg[k][1]
        own=frozenset(starts_)
        stops=ALLBEGINS-own
        seeds=list(starts_)
        for e in [x for x in EXTRA.get(L['root'],[])]: seeds.append(e)
        r=C.analyze2(seeds,allowed,stops,own)
        # extra entry points that are only reached by the EH machinery (SEH __except JumpTargets, catch-continuation labels taken with lea)
        ex2=set(t for s,t,m in r['rip_code'] if allowed(t) and t not in r['insn'])|set(jt for jt in SEHJT.get(L['root'],[]) if allowed(jt) and jt not in r['insn'])
        if ex2:
            r=C.analyze2(seeds+sorted(ex2),allowed,stops,own); r['extra_seeds']=sorted(ex2)
        ins=r.pop('insn')
        # data ranges from jump tables
        data=[]
        for jt in r['jtabs']:
            data.append((jt['table'],jt['table']+4*jt['n'],'jumptable'))
            bt=jt['byte_table']
            if bt[0] is not None and bt[1]: data.append((bt[0],bt[0]+bt[1],'jt-bytes'))
        r['data']=data
        # coverage
        cov=bytearray()
        total=sum(b-a for a,b in rg)
        covered=set()
        for a in ins: pass
        cb=sum(ins.values())
        # bytes not covered: collect ranges
        mark=[]
        for a,b in rg:
            p=a
            ia=sorted(x for x in ins if a<=x<b)
            for x in ia:
                if x>p: mark.append((p,x))
                p=max(p,x+ins[x])
            if p<b: mark.append((p,b))
        # subtract data
        un=[]
        for a,b in mark:
            segs=[(a,b)]
            for da,db,_ in data:
                ns=[]
                for sa,sb in segs:
                    if db<=sa or da>=sb: ns.append((sa,sb)); continue
                    if sa<da: ns.append((sa,da))
                    if db<sb: ns.append((db,sb))
                segs=ns
            un+=segs
        # byte tables whose length is unknown: extend over the unreached chunk that starts at the table
        for jt in r['jtabs']:
            bt=jt['byte_table']
            if bt[0] is not None and not bt[1]:
                for ua,ub in un:
                    if ua==bt[0]:
                        data.append((ua,ub,'jt-bytes-inferred')); un=[x for x in un if x!=(ua,ub)]; break
        r['data']=data
        r['unreached']=un
        r['insn_starts']=sorted(ins)
        r['insn_n']=len(ins)
        res[L['root']]=r
    return res
if __name__=='__main__':
    t0=time.time()
    N=96; chunks=[LOG[i::N] for i in range(N)]
    with mp.Pool(16) as p: parts=p.map(work,chunks)
    R={}
    for d in parts: R.update(d)
    pickle.dump(dict(R=R,LOG=LOG),open('cfg_pdata.pkl','wb'))
    print('done',len(R),'insns',sum(v['insn_n'] for v in R.values()))
    ub=sum(b-a for v in R.values() for a,b in v['unreached']); print('unreached bytes',ub,'in funcs',sum(1 for v in R.values() if v['unreached']))
    print('jtabs',sum(len(v['jtabs']) for v in R.values()),'susp',sum(len(v['susp_jt']) for v in R.values()),'unres',sum(len(v['unres_ijmp']) for v in R.values()))
    print('fall_out',sum(len(v['fall_out']) for v in R.values()),'fall_stop',sum(len(v['fall_stop']) for v in R.values()),'bad_seeds',sum(len(v['bad_seeds']) for v in R.values()))
