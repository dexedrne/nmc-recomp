# PSO key: exact decode (kh2fm.exe, sha256 9002b2de6a1f91a7...)

Status: **bit layout exact and machine-verified** against the code (see Verification). The previous partial decode in `scratch/S1-graphics-api/pso_key_decode.json` had several errors (bit 21/22 meaning, shader-pair stride/count, the RT format "select", the alpha-blend and stencil dwords); this document supersedes it. Decoder: `pso_key.py` (`python3 pso_key.py 0008002e:00000000:00007e00`). Machine-readable: `pso_key_table.json`.

## 1. Where the key lives

| Item | Address / location |
|---|---|
| Key consumer, builds `D3D12_GRAPHICS_PIPELINE_STATE_DESC` (0x290 B, memset) and calls `ID3D12Device::CreateGraphicsPipelineState` (`[dev+0x2e0]` vtbl+0x50, IID `{765a30f3-...}` = ID3D12PipelineState) | fn `0x117710(this=GXDeviceImpl, out ID3D12PipelineState**, key*)`, single caller `0x113590` @0x11363e |
| Cache + bind | fn `0x113590(this, key*)`: red-black `std::map<key16, ID3D12PipelineState*>` at `this+0x59d0`, compared as two u64 (q0 = dw1:dw0, q1 = dw3:dw2); miss -> `0x117710` -> insert; then `cmdlist(0x11bb30)->SetPipelineState` (vtbl +0xc8) |
| Producers (only three; verification: re-verified by independent disassembly, every bit position and the 3 call sites of 0x113590 confirmed) | fn `0x1121d0` (the general draw/state-flush; key built in the chained fragment 0x112fee-0x113584 at 0x11323d-0x1134d4); fn `0x10c7e0` (post-process quad pass, 4-vertex strip); fn `0x10dbc0` (post-process pass, 3 vertices, pair fixed 0x2e) |
| Callers of `0x113590` | exactly 3: 0x113551 (in 0x1121d0), 0x10ccf6 (in 0x10c7e0), 0x10de2c (in 0x10dbc0). No indirect references found (no .rdata/.data pointers to it) |
| Current / previous key / dirty flags | `GXDeviceImpl+0x7730` (16 B), `+0x7740`, byte `+0x7750`, byte `+0x7752`; ctor `0x10a270` zeroes both and sets dirty |
| Pair table the key indexes | `GXDeviceImpl+0x7bf8`, 48 records x 0x228 B (see SHADERS.md section 4) |

The key is **128 bits wide but only 96 bits are ever non-zero**: dw3 is written 0 by every producer (the 16-byte store copies a zeroed qword). The task description's "96-bit key" is therefore right in effect.

## 2. Bit layout

```
dw0  bit 0-6   shader_pair        index into the 48-record pair table (VS blob, PS blob, input layout)
     bit 7     blend_flag         1: use blend op/src/dst of bits 8-18 ; 0: desc gets ADD / ONE / ZERO
     bit 8-10  blend_op           D3D12_BLEND_OP raw (1 ADD .. 5 MAX)
     bit 11-14 src_blend          D3D12_BLEND raw (1 ZERO 2 ONE 3 SRC_COLOR 4 INV_SRC_COLOR 5 SRC_ALPHA 6 INV_SRC_ALPHA 7 DEST_ALPHA 8 INV_DEST_ALPHA 9 DEST_COLOR 10 INV_DEST_COLOR 11 SRC_ALPHA_SAT 14 BLEND_FACTOR 15 INV_BLEND_FACTOR)
     bit 15-18 dst_blend          same
     bit 19-20 cull               D3D12_CULL_MODE raw (1 NONE 2 FRONT 3 BACK)  -- raw value 0 would be an invalid desc
     bit 21    dsv_present        = NOT bit31 of device word [dev+0x78]; gates BOTH depth and stencil enable
     bit 22    depth_test         = [dev+0x260] & 1
     bit 23    depth_write        = [dev+0x270] & 1 (set only when bit 22's source is nonzero)
     bit 24-26 depth_func - 1     D3D12_COMPARISON_FUNC - 1 (so 0 = NEVER ... 7 = ALWAYS)
     bit 27-28 sample_log2        0..3 = SampleDesc.Count 1/2/4/8
     bit 29    alpha_flag         1: use alpha op/src/dst from dw1 bits 0-10 ; 0: ADD / ONE / ZERO
     bit 30-31 unused (0)
dw1  bit 0-2   alpha_op           BlendOpAlpha raw
     bit 3-6   alpha_src          SrcBlendAlpha raw
     bit 7-10  alpha_dst          DestBlendAlpha raw
     bit 11    stencil_flag       = [dev+0x218] & 1
     bit 12-19 stencil_read_mask  byte [dev+0x230]
     bit 20-27 stencil_write_mask byte [dev+0x234]
     bit 28-30 stencil_fail - 1   D3D12_STENCIL_OP - 1 (stored only if [dev+0x218] != 0)
     bit 31    unused (0)
dw2  bit 0-2   stencil_depthfail - 1
     bit 3-5   stencil_pass - 1
     bit 6-8   stencil_func - 1   (comparison func - 1)
     bit 9-12  write_mask         D3D12_COLOR_WRITE_ENABLE (R=1 G=2 B=4 A=8)
     bit 13-15 topology_type      D3D12_PRIMITIVE_TOPOLOGY_TYPE raw (producers emit 2 LINE or 3 TRIANGLE only)
dw3  0
```

### How the fields map to the D3D12 desc (0x117710, verified bit-exact, 4000 random keys)

Desc offsets: pRootSignature +0x00 = `[dev+0x478]`; VS +0x08 / PS +0x18 = pair-record blobs (`GetBufferPointer` vtbl+0x18, `GetBufferSize` +0x20); BlendState RT0 (BlendEnable +0x80, Src +0x88, Dest +0x8c, Op +0x90, SrcAlpha +0x94, DestAlpha +0x98, OpAlpha +0x9c, WriteMask +0xa4); SampleMask +0x1c0; Rasterizer: Fill +0x1c4, Cull +0x1c8, DepthClip +0x1dc; DepthStencil: DepthEnable +0x1f0, WriteMask +0x1f4, Func +0x1f8, StencilEnable +0x1fc, masks +0x200/+0x201, front faces +0x204..+0x213, back faces +0x214..+0x223; InputLayout +0x228 (ptr = record+0x20, count = record+0x220); PrimitiveTopologyType +0x23c; NumRenderTargets +0x240; RTVFormats[0] +0x244; DSVFormat +0x264; SampleDesc +0x268.

| Desc field | Value |
|---|---|
| BlendEnable | `(dw0 & 0x20000080) != 0` (bit 7 **or** bit 29) |
| Src/Dest/Op | bit 7 set: bits 8-18; else ONE / ZERO / ADD |
| SrcAlpha/DestAlpha/OpAlpha | bit 29 set: dw1 bits 0-10; else ONE / ZERO / ADD |
| RenderTargetWriteMask | dw2 bits 9-12 |
| CullMode | dw0 bits 19-20 (raw) |
| DepthEnable | `(dw0 & 0x600000) == 0x600000` |
| DepthWriteMask | dw0 bit 23 |
| DepthFunc | dw0 bits 24-26 + 1 |
| StencilEnable | dw1 bit 11 AND dw0 bit 21 |
| StencilRead/WriteMask | dw1 bits 12-19 / 20-27 (written for every key, zero when not populated) |
| Front and back stencil ops | identical: Fail = dw1 28-30 + 1, DepthFail = dw2 0-2 + 1, Pass = dw2 3-5 + 1, Func = dw2 6-8 + 1 |
| PrimitiveTopologyType | dw2 bits 13-15 |
| SampleDesc.Count | `{1,2,4,8}[dw0 bits 27-28]`, Quality 0 |
| DSVFormat | `0x14` (D32_FLOAT_S8X24_UINT) if DepthEnable or StencilEnable, else 0 |
| Constants | NumRenderTargets 1, **RTVFormats[0] = 0x57 (B8G8R8A8_UNORM) always**, SampleMask 0xffffffff, FillMode SOLID, FrontCounterClockwise 0, DepthBias 0, DepthClipEnable 1, MultisampleEnable 0, AlphaToCoverage 0, IndependentBlend 0, LogicOp none, IBStripCutValue disabled, NodeMask 0, no cached PSO, Flags 0 |

Consequences for the Metal port: one colour attachment format (BGRA8) and one depth-stencil format (D32_FLOAT_S8X24), so the **render-pass pixel formats are static**; PSO variation is only blend, depth/stencil, cull, MSAA count, topology class and shader pair. There is no GPU-visible logic-op, no alpha-to-coverage, no depth bias.

## 3. Producers in detail

### 3.1 General draw (fn 0x1121d0, fragment key build)
Device (`this`=GXDeviceImpl) fields read: shader `[0x60]`, cull `[0x84]`, flags `[0x78]`, blend-enable byte `[0x180]` + struct `{op,src,dst,constByte}` `[0x184..0x193]`, alpha-blend-enable byte `[0x1a8]` + struct `[0x1ac..0x1bb]`, stencil struct `{enable,func,failop,depthfailop,passop,ref@0x22c,readmask@0x230,writemask@0x234}` `[0x218..0x237]`, depth `[0x260]` test byte, `[0x264]` func, `[0x270]` write byte, colour FBMASK dword `[0x274]` (PS2-style: a zero byte means the channel is written), GX primitive mode `[0x7758]`, RT-set record sample count (selected by `[0x6c]`: 0 -> `[0x5e0]`, 5 -> `[0x700]`, n -> `[0x728 + n*0x120]`). Side effects in the same function: `OMSetBlendFactor` (vtbl 0xb8) with `constByte/255` (colour) and the alpha struct's byte/255 when the blend factors are BLEND_FACTOR(14/15); `OMSetStencilRef` (0xc0) = `[0x22c]`.
Reachability: the setters are GX entry functions (list in `pso_key_table.json` `state_setters`) that take **raw D3D12 enum values**; e.g. `0x125ca0 setBlend(op,src,dst,const)`, `0x126200 setCull`, `0x127030 setShader`. A PS2-GS-style translation front end `0x4a6ca0 setBlendMode(opIdx,srcIdx,dstIdx,fix)` maps the GS ALPHA selectors through tables at 0x5ead50-0x5eada0 (op {1,2,3,4,5}; src[0..10]={9,10,5,6,7,8,5,6,-1,-1,14}; dst[0..10]={3,4,5,6,7,8,5,6,-1,-1,14}).
GX primitive mode `[0x7758]` (the draw-call argument, table at 0x5a9d10 gives the IA topology): modes {2,4} -> LINE PSO type; **every other mode, including 1 (point), produces TRIANGLE**; the IA table maps mode 1 to POINTLIST, which would mismatch a TRIANGLE PSO under real D3D12, so point primitives are presumably never drawn (unverified).

### 3.2 Post-process passes
- `0x10c7e0`: `dw0 = (blend op/src/dst copied from [0x184..] state) | idx<<0 (idx = [obj+4] & 0x7f) | cull NONE (bit 19) | dsv_present | sample bits`, `dw1` = alpha op/src/dst copied, `dw2 = 0x7e00` (write mask F, TRIANGLE). **Neither bit 7 nor bit 29 is ever set**, so the copied blend fields are dead: the effective PSO is blend OFF, no depth, no stencil. Draw = `IASetPrimitiveTopology(5)`, `DrawInstanced(4,1,0,0)`.
- `0x10dbc0`: same with pair fixed `0x2e` (= `0x8002e` OR-ed in), `DrawInstanced(3,1,0,0)`.
These are the fullscreen passes using pairs 45 (NV12 movie), 46 (copy), 47 (final composition) with the vertex-less VS 0x59ac40.

## 4. Static table of reachable keys

A complete enumeration of reachable key *values* is not possible from static analysis alone because blend/depth/shader state is set by ~2,300 GX call sites with computed arguments. What the static analysis does give (all in `pso_key_table.json`):
1. The exact **value domains** per field (`value_domains`).
2. The full **shader-pair table** (46 populated pairs: VS/PS blob RVAs, input layouts, strides) - this is the axis that multiplies PSO count (<= 46 x state combos).
3. **Static immediate arguments** at every call site of the setters (`state_setters.static_immediate_call_sites`): e.g. `setShader` immediates {2,3,4,6,7,8,11,13,27,31,33,34,35,36,38} (+34 computed, 11 unresolved of 122 sites), `setCull` immediates {1 NONE, 3 BACK} (+computed), `setColorMask(0x00ffffff)` (RGB bytes non-zero => RGB masked, alpha byte zero => alpha written: write mask 8), `setAlphaBlend(op=1 ADD, src=14 BLEND_FACTOR, ...)` and `setAlphaBlend(dyn, src=2 ONE, dyn, const 0)`, `setAlphaBlendEnable` 0/1, `setDepthTest` 0/1, `setDepthWrite(0)`.
4. Example keys (`example_keys`), e.g. `0008002e:00000000:00007e00` = pair 46, no blend, no depth, cull NONE, mask F, TRIANGLE.
Runtime-only: the actual set of distinct keys ever built (the PSO cache size) needs one runtime capture of `0x113590` (owner-gated).

## 5. Verification (method and results)

Isolated functions of the exe were run in Unicorn on fake in-memory device objects (no game state, no I/O; `tools/emu.py`): the producer fragment (4000 random device states), `0x10c7e0` and `0x10dbc0` prefixes (3000 each), and the consumer `0x117710` (4000 random 96-bit keys, incl. invalid enum values, with fake blob objects and a hooked `CreateGraphicsPipelineState`). Each was compared field-by-field with `pso_key.py`: **0 mismatches** in all four suites (`tools/verify_key_main.py`, `verify_key_post.py`, `verify_consumer.py`). Note: Unicorn executed fragments/prefixes of game code only as a dev oracle for this decode; it is not a run of the game.

## 6. Open items
- Meaning of the two small ints in each pair record (+0x14, +0x18; values 0-2 / 1-2).
- Whether pairs 0 and 1 are ever referenced (never populated; a draw with them would crash in `0x117710`).
- Which GX/game classes select which pair (call-site fns listed in SHADERS.md; classes unnamed).
