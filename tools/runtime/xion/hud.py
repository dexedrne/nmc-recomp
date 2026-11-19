"""Assemble the three portraits in the target SEQD atlas and patch only its IMGD."""
from pathlib import Path
import numpy as np
from PIL import Image
from mdlx import OBJ, R, directory
from models import OUT, STEMS

SLOTS=(36,47,38,47,5,41,3,36,47,0,36,36,38,39,41)
RUN=OUT.parents[3]/'run-O56'

def expression(portrait,state):
    if not state: return portrait
    # Compress the painted eyes between the lids in the two angled renders.
    eyes=[(104,138),(171,138)] if state==1 else [(87,163),(155,165)]
    rgba=np.array(portrait)
    y,x=np.indices(rgba.shape[:2])
    for cx,cy in eyes:
        dy=y-cy
        edge=np.clip((21-abs(x-cx))/6,0,1)
        offset=np.interp(dy,[-17,-1,0,1,17],[-17,-8,0,8,17])-dy
        source=np.clip(np.rint(y+np.where(abs(dy)<17,offset*edge,0)).astype(int),0,len(rgba)-1)
        rgba=rgba[source,x]
    if state==2:
        band=np.exp(-((y-161)/24)**2)[:,:,None]
        rgba[:,:,:3]=rgba[:,:,:3]*(1-band*np.array([.49,.38,.22]))
    return Image.fromarray(rgba)

def sheet(height):
    image=Image.new('RGBA',(1024,height))
    for i,x in enumerate((140,420,710)):
        portrait=Image.open(RUN/f'portrait-{i}.png').convert('RGBA')
        portrait=expression(portrait,i)
        portrait=portrait.crop(portrait.getbbox())
        portrait.thumbnail((237,310),Image.Resampling.LANCZOS)
        rgba=np.array(portrait)
        if i:
            # Injured and exhausted faces retain the angled pose under a status tint.
            strength=np.linspace(1,.25,len(rgba))[:,None,None]
            tint=np.array([.13,-.09,-.10] if i==1 else [-.38,-.22,.04])
            rgba[:,:,:3]=np.clip(rgba[:,:,:3]*(1+strength*tint),0,255)
        image.alpha_composite(Image.fromarray(rgba),(x-portrait.width//2,319-portrait.height))
    return image

def patch_face(data,image,member=b'face'):
    _,_,_,off,size=next(e for e in directory(data) if e[0]==24 and e[2]==member)
    buf,n,pal,pn=R('<4I',data,off+8)
    w,h=R('<HH',data,off+28)
    assert n==w*h and pn==1024 and buf+n<=size and pal+pn<=size
    q=image.resize((w,h),Image.Resampling.LANCZOS).quantize(256,Image.Quantize.FASTOCTREE)
    idx=np.asarray(q)
    colors=np.array(q.getpalette('RGBA'),dtype=np.uint8).reshape(-1,4)
    colors[:,3]=(colors[:,3].astype('u2')+1)//2
    indices=np.arange(len(colors))
    permutation=(indices&0xe7)|((indices&8)<<1)|((indices&16)>>1)
    palette=np.zeros((256,4),dtype=np.uint8)
    palette[permutation]=colors
    y,x=np.indices((h,w))
    if R('<I',data,off+60)[0]&4:
        address=(y&~15)*w+(x&~15)*2+(((y&~3)>>1)+(y&1))%8*w*2+((x+(((y+2)>>2)&1)*4)&7)*4+((y>>1)&1)+((x>>2)&2)
    else: address=y*w+x
    pixels=np.empty(n,dtype=np.uint8)
    pixels[address]=idx
    assert np.array_equal(pixels[address],idx) and len(np.unique(address))==n
    result=bytearray(data)
    result[off+buf:off+buf+n]=pixels.tobytes()
    result[off+pal:off+pal+pn]=palette.tobytes()
    return result

if __name__=='__main__':
    for stem,slot in zip(STEMS,SLOTS,strict=True):
        name=stem+'.a.us'
        original=OBJ.parent/'remastered/obj'/name/f'-{slot}.dds'
        with Image.open(original) as source: height=source.height if source.width==1024 else 512
        atlas=sheet(height)
        dds=OUT.parent/'remastered/obj'/name/original.name
        dds.parent.mkdir(parents=True,exist_ok=True)
        atlas.save(dds)
        assert np.array_equal(np.asarray(Image.open(dds).convert('RGBA')),np.asarray(atlas))
        (OUT/name).write_bytes(patch_face((OBJ/name).read_bytes(),atlas))
        print(name,slot,dds.stat().st_size,flush=True)
