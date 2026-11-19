"""Rebase unhooded Xion rigid VIF coordinates, preserving packet arrangement and strips."""
from mdlx import Archive,Model,P,R,load,np,Path
from mdlx import world, OBJ
import hashlib
import struct

STEMS = """P_EX100 P_EX100_BTLF P_EX100_MAGF P_EX100_KH1F P_EX100_TRIF P_EX100_ULTF
P_EX100_HTLF P_EX101 P_EX101_BTLF P_EX110_BTLF P_EX121 P_EX100_WI P_EX100_WI_MAGF
P_EX100_WI_KH1F P_EX100_WI_ULTF""".split()
OUT = OBJ.parents[1]/'private/looks/xion/obj'


# Map to the actual deforming members of the player chains (85/177 wrist, 86/178 sockets).
JOINTS=np.array([4,0,65,66,67,75,78,79,79,82,85,88,91,92,94,95,96,98,99,100,
                 102,103,104,106,107,108,86,110,137,140,167,170,171,171,174,177,
                 180,183,184,186,187,188,190,191,192,194,195,196,198,199,200,178,
                 202,2,11,12,15,18,40,41,44,47])

def build(stem, output=OUT, joints=JOINTS):
    shell,ally=load(stem),load('N_PO010_BTL')
    body=shell.members[4]
    donor=Model(ally.members[4])
    nxt,count,_,bo,aux=R('<IHHII',body,144+12)
    bones=body[144+bo:144+bo+count*64]
    records=list(struct.iter_unpack('<4hii12f',bones))
    rest=world(np.array([r[6:] for r in records]).reshape(-1,3,4),[r[2] for r in records])
    assert bo-aux == donor.bo-donor.aux
    raw=bytearray(ally.members[4])
    h=144
    # Exclude the ally-only shadow. Match the recipient's null next-model pointer.
    del raw[h+R('<I',raw,h+12)[0]:]
    P('<I',raw,h+12,0)
    anchors=[2,3,4,5,6,7,10,*range(11,26),28,29,30,31,32,35,*range(36,51),54,55,56,57,58,59,60,61]
    source=donor.rest[anchors,:3,3]
    delta=rest[joints[anchors],:3,3]-source
    inverse=np.linalg.inv(rest)
    shift=len(bones)-len(donor.bones)
    for i,(mat,packets) in enumerate(donor.groups):
        hdr=h+32+32*i
        dma,pal=R('<II',raw,hdr+16)
        P('<II',raw,hdr+16,dma+shift,pal+shift)
        for p in packets:
            assert p.xyz_offset is not None and not p.mix
            xyz=np.einsum('nij,nj->ni',donor.rest[p.joints],p.local)
            # Continuous displacement keeps duplicate seam vertices coincident. Six nearest
            # landmarks keep the face from inheriting distant arm or leg displacements.
            dist=np.sum((xyz[:,None,:3]-source)**2,axis=2)
            near=np.argsort(dist,axis=1)[:,:6]
            weights=1/np.maximum(np.take_along_axis(dist,near,axis=1),1e-8)**1.5
            xyz[:,:3]+=np.einsum('nk,nkj->nj',weights/weights.sum(axis=1)[:,None],delta[near])
            local=np.einsum('nij,nj->ni',inverse[joints[p.joints]],xyz)
            raw[p.xyz_offset:p.xyz_offset+12*len(local)]=local[:,:3].astype('<f4').tobytes()
            for tag,listing in p.edits:
                target=joints[R('<I',raw,listing)[0]]
                for pos in (tag,listing): P('<I',raw,pos,target)
            P('<I',raw,p.first+4,R('<I',raw,p.first+4)[0]+shift)
    raw[h+donor.aux:h+donor.bo]=shell.members[4][h+aux:h+bo]
    raw[h+donor.bo:h+donor.bo+len(donor.bones)]=bones
    P('<H',raw,h+16,count)
    if nxt:
        raw.extend(bytes(-len(raw)%16))
        P('<I',raw,h+12,len(raw)-h)
        raw.extend(body[h+nxt:])
    result=Model(raw)
    points=result.mesh(result.rest)[0]
    bounds=np.frombuffer(raw,dtype='<f4',count=8,offset=h+donor.aux).reshape(2,4)
    bounds[0,:3]=np.minimum(bounds[0,:3],points.min(0))
    bounds[1,:3]=np.maximum(bounds[1,:3],points.max(0))
    shell.members.update({4:bytes(raw),7:ally.members[7]})
    out=output/(stem+'.mdlx')
    out.write_bytes(shell.write())
    reread=Model(Archive(out.read_bytes()).members[4])
    assert reread.bones==bones and len(reread.groups)==len(donor.groups)
    for (_,ps),(_,qs) in zip(donor.groups,reread.groups):
        for p,q in zip(ps,qs):
            assert np.array_equal(p.nodes,q.nodes) and np.array_equal(joints[p.joints],q.joints)
    original=load(stem)
    packed=Archive(out.read_bytes())
    assert all(v==packed.members[k] for k,v in original.members.items() if k not in (4,7))
    if nxt: assert packed.members[4][h+R('<I',raw,h+12)[0]:]==body[h+nxt:]
    print(stem,out.stat().st_size,hashlib.sha256(bones).hexdigest()[:8],sum(len(ps) for _,ps in reread.groups),flush=True)

if __name__=='__main__':
    OUT.mkdir(parents=True,exist_ok=True)
    for stem in STEMS: build(stem)
