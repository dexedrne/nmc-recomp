#!/usr/bin/env python3
"""Per .ard file: BAR entries (type, name, size, md5 prefix) as TSV. usage: barstat.py DIR"""
import struct,sys,os,hashlib
root=sys.argv[1]
for f in sorted(os.listdir(root)):
    if not f.lower().endswith('.ard'): continue
    b=open(os.path.join(root,f),'rb').read()
    if b[:4]!=b'BAR\x01': continue
    n=struct.unpack_from('<I',b,4)[0]
    for i in range(n):
        t,d,nm,o,s=struct.unpack_from('<HH4sII',b,16+i*16)
        print('%s\t%d\t%s\t%d\t%s'%(f,t,nm.rstrip(b'\0').decode('latin1'),s,hashlib.md5(b[o:o+s]).hexdigest()[:10]))
