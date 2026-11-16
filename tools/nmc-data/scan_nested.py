import sys,json,hashlib
from kh2lib import *
from kh2lib import _h32,_h16
which,iso,topidx,out=sys.argv[1:5]
top=parse_idx(open(topidx,'rb').read())
tk={(e[0],e[1]):e for e in top}
f=open(iso,'rb'); imgoff,_=iso_extent(f,'KH2.IMG')
entries={}  # key -> dict
for e in top: entries[f'{e[0]:08x}.{e[1]:04x}']=dict(src='top',bd=e[2],sector=e[3],length=e[4])
nested_counts={}
for nm in INTERNAL:
    k=(_h32(nm+'.idx'),_h16(nm+'.idx'))
    if k not in tk: print('missing',nm,file=sys.stderr); continue
    data=read_entry(f,imgoff,tk[k],True)
    ne=parse_idx(data); nested_counts[nm]=len(ne)
    for e in ne:
        key=f'{e[0]:08x}.{e[1]:04x}'
        d=dict(src=nm,bd=e[2],sector=e[3],length=e[4])
        if key in entries and entries[key]['src']!='top': d['dup_of']=entries[key]['src']
        if key not in entries or entries[key]['src']!='top': entries[key]=d
print(which,'total',len(entries),'nested',nested_counts,file=sys.stderr)
cache={}
for key,d in sorted(entries.items(),key=lambda kv:kv[1]['sector']):
    e=(0,0,d['bd'],d['sector'],d['length']); s,sz,comp=span(e); ck=(s,sz,comp)
    if ck not in cache:
        f.seek(imgoff+s); data=f.read(sz)
        if comp: data=data.rstrip(b'\0')
        cache[ck]=hashlib.sha1(data).hexdigest()
    d['sha1']=cache[ck]; d['comp']=comp
json.dump(dict(entries=entries,nested_counts=nested_counts),open(out,'w'))
print(which,'hashed',len(entries),file=sys.stderr)
