#!/usr/bin/env python3
"""KH2FM PC (kh2fm.exe sha256 9002b2de...) 128-bit D3D12 PSO cache key: pack / unpack / to D3D12 desc.
Derived from disassembly of 0x117710 (consumer) and 0x1121d0 / 0x10c7e0 / 0x10dbc0 (producers).
Key = 4 little-endian dwords (dw3 is always 0). Map key is compared as two u64 (q0=dw1:dw0, q1=dw3:dw2)."""
import struct, sys, json

# D3D12 enum names (values as written to the desc)
BLEND = {1:'ZERO',2:'ONE',3:'SRC_COLOR',4:'INV_SRC_COLOR',5:'SRC_ALPHA',6:'INV_SRC_ALPHA',7:'DEST_ALPHA',
         8:'INV_DEST_ALPHA',9:'DEST_COLOR',10:'INV_DEST_COLOR',11:'SRC_ALPHA_SAT',14:'BLEND_FACTOR',15:'INV_BLEND_FACTOR'}
BLEND_OP = {1:'ADD',2:'SUBTRACT',3:'REV_SUBTRACT',4:'MIN',5:'MAX'}
CULL = {1:'NONE',2:'FRONT',3:'BACK'}
CMP = {1:'NEVER',2:'LESS',3:'EQUAL',4:'LESS_EQUAL',5:'GREATER',6:'NOT_EQUAL',7:'GREATER_EQUAL',8:'ALWAYS'}
STENCIL_OP = {1:'KEEP',2:'ZERO',3:'REPLACE',4:'INCR_SAT',5:'DECR_SAT',6:'INVERT',7:'INCR',8:'DECR'}
TOPO_TYPE = {0:'UNDEFINED',1:'POINT',2:'LINE',3:'TRIANGLE',4:'PATCH'}

def bits(v,lo,n): return (v>>lo)&((1<<n)-1)

def unpack(dw0,dw1,dw2,dw3=0):
    """Return semantic fields exactly as 0x117710 interprets them."""
    f={}
    f['shader_pair']=bits(dw0,0,7)
    f['blend_flag7']=bits(dw0,7,1)            # dw0 bit7
    f['alpha_flag29']=bits(dw0,29,1)          # dw0 bit29
    f['blend_enable']=int(bool(dw0&0x20000080))
    f['blend_op']=bits(dw0,8,3); f['src_blend']=bits(dw0,11,4); f['dst_blend']=bits(dw0,15,4)
    f['cull']=bits(dw0,19,2)
    f['dsv_present']=bits(dw0,21,1); f['depth_test']=bits(dw0,22,1)
    f['depth_enable']=int((dw0&0x600000)==0x600000)
    f['depth_write']=bits(dw0,23,1); f['depth_func']=bits(dw0,24,3)+1
    f['sample_log2']=bits(dw0,27,2); f['sample_count']=[1,2,4,8][bits(dw0,27,2)]
    f['alpha_op']=bits(dw1,0,3); f['alpha_src']=bits(dw1,3,4); f['alpha_dst']=bits(dw1,7,4)
    f['stencil_flag']=bits(dw1,11,1)
    f['stencil_enable']=int(bool(bits(dw1,11,1) and bits(dw0,21,1)))
    f['stencil_read']=bits(dw1,12,8); f['stencil_write']=bits(dw1,20,8)
    f['stencil_fail']=bits(dw1,28,3)+1
    f['stencil_depthfail']=bits(dw2,0,3)+1; f['stencil_pass']=bits(dw2,3,3)+1; f['stencil_func']=bits(dw2,6,3)+1
    f['write_mask']=bits(dw2,9,4)
    f['topology_type']=bits(dw2,13,3)
    return f

def to_desc(dw0,dw1,dw2,dw3=0):
    """Mirror of 0x117710: the D3D12_GRAPHICS_PIPELINE_STATE_DESC fields the key controls. All others constant."""
    f=unpack(dw0,dw1,dw2,dw3)
    d={}
    if dw0&0x80:
        d['BlendOp']=f['blend_op']; d['SrcBlend']=f['src_blend']; d['DestBlend']=f['dst_blend']
    else:
        d['BlendOp']=1; d['SrcBlend']=2; d['DestBlend']=1
    if dw0&(1<<29):
        d['BlendOpAlpha']=f['alpha_op']; d['SrcBlendAlpha']=f['alpha_src']; d['DestBlendAlpha']=f['alpha_dst']
    else:
        d['BlendOpAlpha']=1; d['SrcBlendAlpha']=2; d['DestBlendAlpha']=1
    d['BlendEnable']=f['blend_enable']
    d['RenderTargetWriteMask']=f['write_mask']
    d['CullMode']=f['cull']
    d['DepthEnable']=f['depth_enable']; d['DepthWriteMask']=f['depth_write']; d['DepthFunc']=f['depth_func']
    d['StencilEnable']=f['stencil_enable']
    if f['stencil_enable']:
        d['StencilReadMask']=f['stencil_read']; d['StencilWriteMask']=f['stencil_write']
        for face in('Front','Back'):
            d[face+'FaceStencilFailOp']=f['stencil_fail']; d[face+'FaceStencilDepthFailOp']=f['stencil_depthfail']
            d[face+'FaceStencilPassOp']=f['stencil_pass']; d[face+'FaceStencilFunc']=f['stencil_func']
    d['PrimitiveTopologyType']=f['topology_type']
    d['SampleDescCount']=f['sample_count']
    d['DSVFormat']=0x14 if (f['depth_enable'] or f['stencil_enable']) else 0   # D32_FLOAT_S8X24_UINT
    d['RTVFormats0']=0x57; d['NumRenderTargets']=1                            # B8G8R8A8_UNORM, always
    d['SampleMask']=0xffffffff; d['FillMode']=3; d['DepthClipEnable']=1
    d['ShaderPair']=f['shader_pair']
    return d

def pack(shader_pair=0, blend_flag=0, blend_op=1, src=2, dst=1, cull=1, dsv_present=0, depth_test=0, depth_write=0,
         depth_func=1, sample_log2=0, alpha_flag=0, alpha_op=1, alpha_src=2, alpha_dst=1, stencil_flag=0,
         stencil_read=0, stencil_write=0, stencil_fail=1, stencil_depthfail=1, stencil_pass=1, stencil_func=1,
         write_mask=0xf, topology_type=3):
    dw0=(shader_pair&0x7f)|(blend_flag<<7)|((blend_op&7)<<8)|((src&0xf)<<11)|((dst&0xf)<<15)|((cull&3)<<19)
    dw0|=(dsv_present<<21)|(depth_test<<22)|(depth_write<<23)|(((depth_func-1)&7)<<24)|((sample_log2&3)<<27)|(alpha_flag<<29)
    dw1=(alpha_op&7)|((alpha_src&0xf)<<3)|((alpha_dst&0xf)<<7)|(stencil_flag<<11)|((stencil_read&0xff)<<12)|((stencil_write&0xff)<<20)|(((stencil_fail-1)&7)<<28)
    dw2=((stencil_depthfail-1)&7)|(((stencil_pass-1)&7)<<3)|(((stencil_func-1)&7)<<6)|((write_mask&0xf)<<9)|((topology_type&7)<<13)
    return dw0&0xffffffff,dw1&0xffffffff,dw2&0xffffffff,0

def describe(dw0,dw1,dw2,dw3=0):
    f=unpack(dw0,dw1,dw2,dw3); d=to_desc(dw0,dw1,dw2,dw3)
    s='pair=%d'%f['shader_pair']
    s+=' blend=%s'%('off' if not d['BlendEnable'] else '%s(%s,%s)'%(BLEND_OP.get(d['BlendOp']),BLEND.get(d['SrcBlend']),BLEND.get(d['DestBlend'])))
    if d['BlendEnable']: s+=' alpha=%s(%s,%s)'%(BLEND_OP.get(d['BlendOpAlpha']),BLEND.get(d['SrcBlendAlpha']),BLEND.get(d['DestBlendAlpha']))
    s+=' cull=%s'%CULL.get(d['CullMode'],d['CullMode'])
    s+=' depth=%s'%('off' if not d['DepthEnable'] else '%s%s'%(CMP[d['DepthFunc']],'+W' if d['DepthWriteMask'] else ''))
    if d['StencilEnable']: s+=' stencil=%s rd%02x wr%02x f=%s zf=%s p=%s'%(CMP[f['stencil_func']],f['stencil_read'],f['stencil_write'],STENCIL_OP[f['stencil_fail']],STENCIL_OP[f['stencil_depthfail']],STENCIL_OP[f['stencil_pass']])
    s+=' wmask=%x topo=%s msaa=%dx'%(d['RenderTargetWriteMask'],TOPO_TYPE[d['PrimitiveTopologyType']],d['SampleDescCount'])
    return s

if __name__=='__main__':
    # decode keys from args: 8-hex-digit dwords "dw0:dw1:dw2" or a 32-hex-digit LE blob
    for a in sys.argv[1:]:
        if ':' in a: v=[int(x,16) for x in a.split(':')]
        else:
            b=bytes.fromhex(a); v=list(struct.unpack('<4I',b))
        while len(v)<4: v.append(0)
        print('%08x:%08x:%08x:%08x'%tuple(v), describe(*v))
