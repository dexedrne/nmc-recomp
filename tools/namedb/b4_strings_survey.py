import sys,re,collections,pickle
sys.path.insert(0,'/home/user/projects/kh2-nmc-analysis/scratch/namedb2')
from lib import *
im=Img(); img=im.img
SC=pickle.load(open(HERE+'scan.pkl','rb'))
# extract ASCII strings (>=4) from .rdata, .data (initialised part), _RDATA, .shared
strs={}
for name in ('.rdata','.data','_RDATA','.shared'):
    for n,a,v,po,ps,ch in im.secs:
        if n!=name: continue
        end=a+min(v,ps)
        d=img[a:end]
        for m in re.finditer(rb'[\x20-\x7e]{4,}',d):
            s=m.group(0)
            # must be NUL terminated or at least preceded by NUL / alignment
            strs[a+m.start()]=s.decode('ascii')
print(len(strs))
pickle.dump(strs,open(HERE+'strings.pkl','wb'))
# which strings are referenced by code (rip-relative)?
ref=collections.defaultdict(set)
for fn,s in SC.items():
    for (addr,t,mn) in s['refs']:
        ref[t].add(fn)
# string start can be interior; map: for ref target t, find string containing t
import bisect
ks=sorted(strs)
def find(t):
    i=bisect.bisect_right(ks,t)-1
    if i>=0 and ks[i]<=t<ks[i]+len(strs[ks[i]])+1: return ks[i]
    return None
sref=collections.defaultdict(set)
for t,fs in ref.items():
    k=find(t)
    if k is not None: sref[k]|=fs
print('referenced strings',len(sref))
pats={'file':re.compile(r'\.(cpp|c|h|hpp|cc)\b',re.I),'dc':re.compile(r'\w+::\w+'),'tag':re.compile(r'^\[[A-Za-z0-9_ ]{1,12}\]'),'func_colon':re.compile(r'^[A-Za-z_]\w+\s*[:(]'),'assert':re.compile(r'assert|ASSERT|Assert')}
for k,p in pats.items():
    L=[(a,s) for a,s in strs.items() if p.search(s) and a in sref]
    print(k,len(L)); 
    for a,s in L[:12]: print('   ',hex(a),repr(s[:100]),len(sref[a]))
tags=collections.Counter(re.match(r'^\[[^\]]+\]',s).group(0) for a,s in strs.items() if re.match(r'^\[[^\]]{1,16}\]',s))
print(tags.most_common(40))
