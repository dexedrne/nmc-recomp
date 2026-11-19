"""Compare both socket matrices across an actual animation for each bind family."""
import subprocess
import sys
import tempfile
from pathlib import Path
from xml.etree import ElementTree
from mdlx import OBJ, directory, np
from models import OUT

BAKER=Path(__file__).resolve().parents[3]/'khnmc-X4/bake_host.py'
for stem,motion in [('P_EX100','P_EX100'),('P_EX100_BTLF','P_EX100_KH1F'),
                    ('P_EX100_TRIF','P_EX100_TRIF'),('P_EX110_BTLF','P_EX110_BTLF')]:
    mset=(OBJ/(motion+'.mset')).read_bytes()
    _,_,clip,o,n=next(e for e in directory(mset) if e[0]==17 and e[-1])
    with tempfile.TemporaryDirectory(dir='run-O56') as temp:
        temp=Path(temp).resolve()
        anb=temp/'clip.anb'
        anb.write_bytes(mset[o:o+n])
        def sockets(model):
            subprocess.run([sys.executable,'-B',BAKER,'bake',model,anb,temp,'-f','1'],check=True)
            frames={}
            for f in ElementTree.parse(temp/'0.xml').findall('Frame'):
                matrices=np.fromstring(f.findtext('Matrices').replace(',',' '),sep=' ').reshape(-1,4,4)
                frames[f.get('Time')]=matrices[[178,86]]
            assert len(frames)>1 and all(np.isfinite(m).all() for m in frames.values())
            return frames
        a=sockets(OBJ/(stem+'.mdlx'))
        b=sockets(OUT/(stem+'.mdlx'))
        assert a.keys()==b.keys()
        errors=np.max([np.abs(a[t]-b[t]) for t in a],axis=(0,2,3))
        assert np.all(errors==0) and any(np.any(m!=next(iter(a.values()))) for m in a.values())
        print('PASS',stem,motion,clip.decode(),len(a),'frames; socket 178,86 max errors',*errors,flush=True)
