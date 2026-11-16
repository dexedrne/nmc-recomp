"""Small PE helpers (pefile + capstone). Scratch-only."""
import pefile, capstone, struct, hashlib, re, bisect
EXE='/home/user/projects/kh2-nmc-analysis/pc/kh2fm.exe'
BASE=0x140000000
class PE:
    def __init__(self,path=EXE,data=None):
        self.path=path
        self.raw=bytearray(data if data is not None else open(path,'rb').read())
        self.pe=pefile.PE(data=bytes(self.raw),fast_load=True)
        self.img=bytes(self.pe.get_memory_mapped_image())
        self.secs=[(s.Name.rstrip(b'\0').decode(),s.VirtualAddress,s.Misc_VirtualSize,s.PointerToRawData,s.SizeOfRawData) for s in self.pe.sections]
        self.md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64); self.md.detail=False
        self.md.skipdata=True
        self.funcs=[]
        try:
            self.pe.parse_data_directories([pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']])
            self.funcs=sorted((f.struct.BeginAddress,f.struct.EndAddress) for f in self.pe.DIRECTORY_ENTRY_EXCEPTION)
        except Exception: pass
        self.fstarts=[f[0] for f in self.funcs]
    def sec(self,rva):
        for n,v,s,p,r in self.secs:
            if v<=rva<v+max(s,r): return n
    def file_off(self,rva):
        """file offset or None if the RVA is not file-backed (bss / headers beyond raw)."""
        for n,v,s,p,r in self.secs:
            if v<=rva<v+min(max(s,1),r) : return p+rva-v
        return None
    def func(self,rva):
        i=bisect.bisect_right(self.fstarts,rva)-1
        if i>=0 and self.funcs[i][0]<=rva<self.funcs[i][1]: return self.funcs[i]
    def dis1(self,rva,buf=None):
        b=(buf if buf is not None else self.img)[rva:rva+15]
        for i in self.md.disasm(b,rva,1): return i
    def insn_at(self,rva,img=None):
        """instruction containing rva (start found by decoding from function start)."""
        img=img if img is not None else self.img
        f=self.func(rva)
        start=f[0] if f else max(0,rva-64)
        # decode linear from func start
        for i in self.md.disasm(img[start:min(start+0x2000,len(img))],start):
            if i.address<=rva<i.address+i.size: return i
            if i.address>rva: break
def sha256(b): return hashlib.sha256(bytes(b)).hexdigest()
def pat_to_re(p):
    toks=p.split()
    return re.compile(b''.join(b'.' if t=='??' else re.escape(bytes([int(t,16)])) for t in toks),re.S)
