import struct,sys,json
sys.dont_write_bytecode=True
WN={0:'zz',1:'es',2:'tt',3:'di',4:'hb',5:'bb',6:'he',7:'al',8:'mu',9:'po',10:'lk',11:'lm',12:'dc',13:'wi',14:'nm',15:'wm',16:'ca',17:'tr',18:'eh'}
CMD={1:'WorldState=0',2:'WorldState=1',3:'WorldState=2',4:'WorldState=2,clr b3.1',5:'World b3|=2',6:'World b3|=1',7:'WorldState=3(visit++)',8:'Course=0',9:'Course=1',10:'Course=2',11:'Course=3',12:'FlagSet(0x3f08e0)',13:'FlagClr(0x3f08c0)',14:'nop',15:'byte418d='}
def bar(b):
    n=struct.unpack_from('<I',b,4)[0]; o={}
    for i in range(n):
        t,d,nm,off,s=struct.unpack_from('<HH4sII',b,16+i*16); o[nm.rstrip(b'\0').decode()]=(off,s)
    return o
def decode(path):
    b=open(path,'rb').read(); wo,ws=bar(b)['wldf']; d=b[wo:wo+ws]
    t,c=struct.unpack_from('<II',d,0); out=[]
    for i in range(c):
        a,bw,w2,flag,u6,u8,ptr=struct.unpack_from('<BBHHHII',d,8+i*16)
        blocks=[]
        if ptr:
            q=ptr
            while True:
                code,cnt=struct.unpack_from('<HH',d,q)
                if code==0: break
                args=[struct.unpack_from('<H',d,q+4+4*j)[0] for j in range(cnt)]
                blocks.append(dict(file_off=wo+q,cmd=code,name=CMD.get(code,'?'),args=args,args_world=[WN.get(x,x) for x in args])); q+=4+4*cnt
        out.append(dict(i=i,id=a,world=WN.get(bw,bw),b=bw,w2=w2,story_flag=flag,u6=u6,u8=u8,ptr=ptr,entry_off=wo+8+i*16,blocks=blocks))
    return out
if __name__=='__main__':
    for e in decode(sys.argv[1]):
        print(e['i'],'id',e['id'],e['world'],'w2',e['w2'],'flag',e['story_flag'],'u8',e['u8'],[(b['cmd'],b['name'],b['args']) for b in e['blocks']])
