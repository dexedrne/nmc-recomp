import re,json,sys,pefile
sys.path.insert(0,'.')
EXE='/home/user/projects/kh2-nmc-analysis/pc/kh2fm.exe'
SRC='/home/user/projects/kh2-nmc-analysis/scratch/openkh/src/OpenKh.Research.Panacea/OpenKH.cpp'
pe=pefile.PE(EXE)
img=bytearray(pe.get_memory_mapped_image())
BASE=pe.OPTIONAL_HEADER.ImageBase
txt=open(SRC).read()
# parse Hook(name, "pat", "mask");
pat=re.compile(r'Hook\((\w+),\s*"((?:[^"\\]|\\.)*)",\s*"([^"]*)"\);')
def unesc(s):
    out=bytearray();i=0
    while i<len(s):
        if s[i]=='\\' and s[i+1]=='x':
            out.append(int(s[i+2:i+4],16)); i+=4
        else: out.append(ord(s[i])); i+=1
    return bytes(out)
hooks=[]
for m in pat.finditer(txt):
    hooks.append((m.group(1),unesc(m.group(2)),m.group(3), txt[:m.start()].count('\n')+1))
print(len(hooks))
res={}
size=len(img)
for name,p,mask,line in hooks:
    L=len(mask)
    matches=[]
    for a in range(0,size-0x10,0x10):   # panacea steps 0x10 from image base
        ok=True
        for i in range(L):
            if mask[i]!='?' and p[i]!=img[a+i] if a+i<size else True:
                ok=False;break
        if ok: matches.append(a)
    # unaligned search too
    mm=[]
    res[name]=dict(line=line,patlen=L,aligned_matches=matches)
    print(name,[hex(x) for x in matches])
json.dump(res,open('pat_raw.json','w'),indent=1)
