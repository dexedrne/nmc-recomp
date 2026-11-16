import pefile, struct, sys
EXE='/home/user/projects/kh2-nmc-analysis/pc/kh2fm.exe'
BASE=0x140000000
pe=pefile.PE(EXE)
img=bytes(pe.get_memory_mapped_image())
def imports():
    out=[]
    for d in pe.DIRECTORY_ENTRY_IMPORT:
        for i in d.imports:
            out.append((d.dll.decode(), i.name.decode() if i.name else 'ord%d'%i.ordinal, i.address-BASE))
    return out
def dis(rva, n=40, size=0x400):
    import capstone
    md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64); md.detail=False
    k=0
    for ins in md.disasm(img[rva:rva+size], BASE+rva):
        print('%x: %s %s'%(ins.address-BASE, ins.mnemonic, ins.op_str)); k+=1
        if k>=n: break
