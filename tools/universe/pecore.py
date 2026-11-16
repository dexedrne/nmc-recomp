"""Shared PE access for the universe build. Read-only on the exe."""
import pefile, struct, bisect, hashlib
EXE='/home/user/projects/kh2-nmc-analysis/pc/kh2fm.exe'
IMAGEBASE=0x140000000
class PEImg:
    def __init__(self, path=EXE):
        self.path=path
        self.raw=open(path,'rb').read()
        self.sha256=hashlib.sha256(self.raw).hexdigest()
        self.pe=pefile.PE(data=self.raw)
        self.img=bytes(self.pe.get_memory_mapped_image())
        self.base=self.pe.OPTIONAL_HEADER.ImageBase
        self.secs=[(s.Name.rstrip(b'\0').decode(),s.VirtualAddress,s.Misc_VirtualSize,s.PointerToRawData,s.SizeOfRawData,s.Characteristics) for s in self.pe.sections]
        self.exec_secs=[(n,va,va+vs) for n,va,vs,po,ps,ch in self.secs if ch&0x20000000]
        self.entry=self.pe.OPTIONAL_HEADER.AddressOfEntryPoint
        dd=self.pe.OPTIONAL_HEADER.DATA_DIRECTORY
        self.dd={d.name.replace('IMAGE_DIRECTORY_ENTRY_',''):(d.VirtualAddress,d.Size) for d in dd}
    def sec_of(self,rva):
        for n,va,vs,po,ps,ch in self.secs:
            if va<=rva<va+vs: return n
        return None
    def in_exec(self,rva):
        return any(a<=rva<b for _,a,b in self.exec_secs)
    def u32(self,r): return struct.unpack_from('<I',self.img,r)[0]
    def i32(self,r): return struct.unpack_from('<i',self.img,r)[0]
    def u64(self,r): return struct.unpack_from('<Q',self.img,r)[0]
    def u16(self,r): return struct.unpack_from('<H',self.img,r)[0]
