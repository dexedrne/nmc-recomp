#!/usr/bin/env python3
"""Count AreaSettings sub-commands (Event / Jump / ...) per KH2 ARD file. Format per OpenKH docs/kh2/file/type/areadata.md.
usage: ardevents.py DIR [--show FILE]   -> prints one TSV line per .ard: name, events, jumps, programs, event names"""
import struct,sys,os
def bar(b):
    if b[:4]!=b'BAR\x01': return []
    n=struct.unpack_from('<I',b,4)[0]; out=[]
    for i in range(n):
        t,d,nm,o,s=struct.unpack_from('<HH4sII',b,16+i*16); out.append((t,nm.rstrip(b'\0').decode('latin1'),b[o:o+s]))
    return out
def programs(d):
    p=0; out=[]
    while p+4<=len(d):
        pid,ln=struct.unpack_from('<hh',d,p)
        if pid==-1 or ln<4 or p+ln>len(d): break
        out.append((pid,d[p+4:p+ln])); p+=ln
    return out
def ops(pd):
    p=0; out=[]
    while p+4<=len(pd):
        op,argc=struct.unpack_from('<hh',pd,p); p+=4
        if argc<0 or p+4*argc>len(pd): break
        out.append((op,pd[p:p+4*argc])); p+=4*argc
    return out
def settings(a):
    """AreaSettings params: int0 = two shorts, then sub-commands (s16 op, s16 argc, argc ints)"""
    p=4; out=[]
    while p+4<=len(a):
        so,sc=struct.unpack_from('<hh',a,p); p+=4
        if sc<0 or p+4*sc>len(a): out.append((so,None)); break
        out.append((so,a[p:p+4*sc])); p+=4*sc
    return out
def scan(path):
    b=open(path,'rb').read(); ev=[]; jumps=0; progs=0; other={}
    for t,nm,d in bar(b):
        if nm not in('map','btl','evt'): continue
        for pid,pd in programs(d):
            progs+=1
            for op,a in ops(pd):
                if op!=0x0c: continue
                for so,sa in settings(a):
                    if so==0 and sa is not None:
                        name=sa[:4].split(b'\0')[0].decode('latin1','replace')
                        typ=struct.unpack_from('<i',sa,4)[0] if len(sa)>=8 else -1
                        ev.append((nm,pid,typ,name))
                    elif so==1: jumps+=1
                    else: other[so]=other.get(so,0)+1
    return ev,jumps,progs,other
if __name__=='__main__':
    root=sys.argv[1]; show=sys.argv[3] if len(sys.argv)>3 and sys.argv[2]=='--show' else None
    tot=0
    for f in sorted(os.listdir(root)):
        if not f.lower().endswith('.ard'): continue
        ev,j,pr,o=scan(os.path.join(root,f)); tot+=len(ev)
        if show==f:
            for e in ev: print('   ',e)
        print('%s\t%d\t%d\t%d\t%s'%(f,len(ev),j,pr,','.join(sorted({e[3] for e in ev}))[:80]))
    print('TOTAL_EVENTS\t%d'%tot)
