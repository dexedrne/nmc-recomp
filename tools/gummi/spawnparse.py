import struct,sys
sys.dont_write_bytecode=True
def bar(b):
    n=struct.unpack_from('<I',b,4)[0]; out=[]
    for i in range(n):
        t,d,nm,o,s=struct.unpack_from('<HH4sII',b,16+i*16); out.append((t,nm.rstrip(b'\0').decode('latin1'),o,s))
    return out
def objnames(path='/home/user/projects/SillyBillyMod1/00objentry.bin'):
    b=open(path,'rb').read(); n=struct.unpack_from('<I',b,0)[0]; r={}
    for i in range(n):
        o=4+i*0x60; oid=struct.unpack_from('<I',b,o)[0]; r[oid]=b[o+8:o+40].split(b'\0')[0].decode('latin1')
    return r
def spawn(d):
    """list of descriptors with entities"""
    t,c=struct.unpack_from('<II',d,0)
    p=8; ds=[]
    # descriptors are 0x30 each header, followed by arrays
    for i in range(c):
        ty,fl,id_,ne,nev,nw,ret,nsig=struct.unpack_from('<BBHHHHHI',d,p)
        ds.append(dict(type=ty,flag=fl,id=id_,n_ent=ne,n_evt=nev,n_walk=nw,ret=ret,n_sig=nsig,area=d[p+0x1c],entr=d[p+0x1d],off=p))
        p+=0x30
    return ds,p
