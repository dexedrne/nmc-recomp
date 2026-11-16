# KH2FM PC graphics spec: the 47 SM4 DXBC shaders

Source: `kh2fm.exe` (sha256 9002b2de6a1f91a7...), blobs at RVA 0x586160-0x5a6c54 (133,604 B). Extracted to `shaders/<rva>.dxbc`, disassembled with the self-written parser `tools/dxbc_dis.py` to `shaders/asm/<rva>.asm`, summarised in `shaders.json`. Everything is `vs_4_0`/`ps_4_0`, compiler string "Microsoft (R) HLSL Shader Compiler 10.1" (fxc, no DXIL, no SM5 features: no compute, no UAV, no tessellation). At startup `0x11e1d0` runs `D3DStripShader(blob, size, 0, &ID3DBlob)` on each blob (50 strip calls over 47 distinct blobs; some VS are used by several pairs), then registers 46 shader pairs (index 2..47) with `0x111830`.

## 1. Binding model (identical for all shaders)

| Slot | Stage | Name | Size | Root parameter |
|---|---|---|---|---|
| b2 | VS | `cbConstantVS` | 144 B | param 0, root CBV |
| b3 | VS | `cbShaderParam` | 3456 B | param 1, root CBV |
| b4 | VS | `cbViewProjection` | 64 B | param 2, root CBV |
| b6 | PS | `cbConstantPS` | 112 B | param 3, root CBV |
| t0 | PS | `txDiffuse` (Texture2D<float4>) | - | param 5, descriptor table (1 SRV) [verification: was 4] |
| t1 | PS | `txDiffuse1` (movie V/UV plane, 2nd texture) | - | param 6, descriptor table (1 SRV) [verification: was 5] |
| s0 | PS | `samLinear` | - | param 4, descriptor table (1 sampler from the 8-entry sampler heap) [verification: was 6; evidence: fn 0x11df90 param array at rbp-0x50 stride 0x20, param 4 range ptr = rsp+0x70 whose RangeType is 3 = SAMPLER; call sites 0x10cb41-0x10cb73 and 0x112d2c-0x112d61 take the sampler heap [dev+0x4f8] GPU start and pass root index 4] |

Root signature flags = 1 (ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), version 1, no static samplers (verified in `0x11df90`). Only 4 of the 7 b-slot numbers (2,3,4,6) are used; b0/b1/b5 are unused.

### Constant buffer layouts (from RDEF; offsets in bytes)

**cbConstantVS** (144 B)

| Offset | Size | Name | Type |
|---|---|---|---|
| 0 | 16 | `Projection2D` | float4 |
| 16 | 16 | `xyOffset` | float4 |
| 32 | 16 | `MagicScale` | float4 |
| 48 | 4 | `SafeScale` | float |
| 52 | 4 | `FogEnable` | uint |
| 56 | 8 | `__DepthScale` | float2 |
| 64 | 4 | `colorScale` | float |
| 68 | 4 | `alphaScale` | float |
| 72 | 8 | `TPage` | float2 |
| 80 | 64 | `ViewProjection` | float4x4 |

**cbShaderParam** (3456 B)

| Offset | Size | Name | Type |
|---|---|---|---|
| 0 | 16 | `TexScale` | float4 |
| 16 | 16 | `Ambient` | float4 |
| 32 | 16 | `FogParam` | float4 |
| 48 | 80 | `ParamVector` | float4[5] |
| 128 | 64 | `LightMatrix` | float4x4 |
| 192 | 64 | `ColorMatrix` | float4x4 |
| 256 | 64 | `TextureMatrix` | float4x4 |
| 320 | 64 | `WorldMatrix` | float4x4 |
| 384 | 3072 | `LocalProjection` | float4x4[48] |

**cbViewProjection** (64 B)

| Offset | Size | Name | Type |
|---|---|---|---|
| 0 | 64 | `WorldView` | float4x4 |

**cbConstantPS** (112 B)

| Offset | Size | Name | Type |
|---|---|---|---|
| 0 | 16 | `RegionU` | float4 |
| 16 | 16 | `RegionV` | float4 |
| 32 | 4 | `alphaRef` | float |
| 36 | 4 | `PSFLAGS` | uint |
| 40 | 4 | `multiTextureAlpha` | float |
| 44 | 4 | `extTexAlpha` | float |
| 48 | 12 | `FogColor` | float3 |
| 60 | 4 | `inverseGamma` | float |
| 64 | 4 | `colorVisionType` | uint |
| 68 | 4 | `colorVisionSeverity` | uint |
| 80 | 20 | `_pad` | float[2] |

## 2. Semantics that matter for a port

**PSFLAGS (cbConstantPS+36, uint).** Tested in every alpha-tested PS (verified in all 21 PS asm files):
- bit 0: alpha test enabled.
- bits 1-3: test function m = `D3D12_COMPARISON_FUNC - 1` (same encoding as the depth func in the PSO key). Implemented: m=1 LESS (keep a < ref), m=3 LESS_EQUAL, m=4 GREATER (keep a > ref), m=6 GREATER_EQUAL; m=0,2,5,7 do not discard. The compare is a PS2-style `discard` against `alphaRef` (cbConstantPS+32, float) AFTER `colour = tex * vertexColour`.
- bit 24 (0x1000000): replace the final alpha by `extTexAlpha` (cbConstantPS+44). Only present in the 8 PS variants that also read `extTexAlpha` (RGB-only textures / texture formats with no alpha).
- No other PSFLAGS bit is tested by any shader.

**RegionU / RegionV (cbConstantPS+0 / +16, float4 each).** Used only by PS 0x59d320 and 0x59e300 (pairs 6 and 7). They emulate the PS2 GS CLAMP register REGION modes with manual bilinear filtering: x = min offset (texels), y = region size (texels), z = texture dimension in texels (the shader multiplies uv by z and divides by z), w = mode as float: 2.0 = region clamp (clamp to [min, min+size]), 3.0 = region repeat (wrap inside the region with `frac`), anything else = plain (rely on sampler). Four taps at snapped texel centres are blended by hand (`sample` x4 in 0x59d320; 0x59e300 contains 12 `sample` ops, structure not analysed further).

**Other PS constants.** `FogColor` (+48, float3): `rgb = fog*colour + (1-fog)*FogColor` when the interpolated fog factor `FOG0.x` (packed into `v0.z` after TEXCOORD0.xy) is not 1.0. `multiTextureAlpha` (+40): 0x5a3690 lerps t0/t1 by it. `inverseGamma` (+60), `colorVisionType` (+64), `colorVisionSeverity` (+68): only the final-composition PS 0x5a4550 (360 instructions) reads them.

**Vertex shaders.** 2D family (`cbConstantVS` only): GS pixel coordinates -> NDC: `pos = (ceil((p.xy - xyOffset) * MagicScale.xy + MagicScale.zw - 0.5) * Projection2D.xy + (-1,-1)) * (1,-1)`, `z = p.z`, `uv = uv_in * TPage.zw`, `colour = colour_in * (colorScale.xxx, alphaScale)`. 3D family multiplies by `cbViewProjection.WorldView` and `cbConstantVS.ViewProjection`, applies fog (`FogParam`, `FogEnable`, `SafeScale`), ambient/lighting (`Ambient`, `LightMatrix`, `ColorMatrix`) and skinning/vertex blending: BLENDINDICES index (dynamic cb index, flagged `cb_dynamic_index`) into `cbShaderParam.LocalProjection` (3072 B = 48 float4x4) and/or `LightMatrix`. POSITION0..POSITION6 inputs are separate position streams (one per matrix influence; the shader blends them with BLENDWEIGHTn) rather than one bind-pose position. `KICK0` is the PS2 GS vertex-kick (ADC) flag passed through; `NORMALINDEX0` selects a normal in the palette. The fullscreen VS 0x59ac40 generates a quad from `SV_VertexID`.

## 3. All 47 shaders

Pair indices refer to `pso_key_table.json` `shader_pair_table` (dw0 bits 0-6 of the PSO key). Stage, size, instruction count (excluding dcl), discards, texture-sample ops.

| RVA | Stage | Bytes | Instr | Disc | Smp | Pairs | Role (derived from the asm) | Inputs -> Outputs |
|---|---|---|---|---|---|---|---|---|
| 0x586160 | vs | 1448 | 11 | 0 | 0 | 3,5,6,7,22 | screen-space / GS pixel-coordinate path (no WorldView; xyOffset/MagicScale/Projection2D or SafeScale) | POSITION0,COLOR0,TEXCOORD0 -> TEXCOORD0,COLOR0,SV_POSITION0 |
| 0x586710 | vs | 1388 | 9 | 0 | 0 | 4 | screen-space / GS pixel-coordinate path (no WorldView; xyOffset/MagicScale/Projection2D or SafeScale) | POSITION0,COLOR0,TEXCOORD0 -> TEXCOORD0,COLOR0,SV_POSITION0 |
| 0x586c80 | vs | 1328 | 10 | 0 | 0 | 2 | screen-space / GS pixel-coordinate path (no WorldView; xyOffset/MagicScale/Projection2D or SafeScale) | POSITION0,COLOR0 -> COLOR0,SV_POSITION0 |
| 0x5871b0 | vs | 1516 | 13 | 0 | 0 | 8,9 | screen-space / GS pixel-coordinate path (no WorldView; xyOffset/MagicScale/Projection2D or SafeScale) | POSITION0,TEXCOORD0,COLOR0 -> TEXCOORD0,COLOR0,SV_POSITION0 |
| 0x5877a0 | vs | 2956 | 35 | 0 | 0 | 43,44 | 3D (WorldView x ViewProjection); 2 POSITION streams (vertex-blend: positions pre-transformed per influence); outputs FOG | POSITION0,POSITION1,COLOR0,TEXCOORD0 -> TEXCOORD0,FOG0,COLOR0,SV_POSITION0 |
| 0x588330 | vs | 3404 | 48 | 0 | 0 | 23,24,25,26 | 3D (WorldView x ViewProjection); outputs FOG | POSITION0,COLOR0,TEXCOORD0 -> TEXCOORD0,FOG0,COLOR0,SV_POSITION0 |
| 0x589080 | vs | 3556 | 57 | 0 | 0 | 27 | 3D (WorldView x ViewProjection); 1 BLENDINDICES inputs (dynamic index into cbShaderParam, likely the LocalProjection/LightMatrix palette); outputs FOG | POSITION0,TEXCOORD0,BLENDINDICES0 -> TEXCOORD0,FOG0,COLOR0,SV_POSITION0 |
| 0x589e70 | vs | 2828 | 31 | 0 | 0 | 21 | 3D (WorldView x ViewProjection); KICK input (name suggests the PS2 GS ADC/kick flag; use not analysed); outputs FOG | POSITION0,COLOR0,TEXCOORD0,KICK0 -> TEXCOORD0,FOG0,COLOR0,SV_POSITION0 |
| 0x58a980 | vs | 2992 | 35 | 0 | 0 | 17 | 3D (WorldView x ViewProjection); lit (NORMAL); outputs FOG | POSITION0,NORMAL0,COLOR0,TEXCOORD0 -> TEXCOORD0,FOG0,COLOR0,SV_POSITION0 |
| 0x58b530 | vs | 3004 | 36 | 0 | 0 | 13,14 | 3D (WorldView x ViewProjection); lit (NORMAL); outputs FOG | POSITION0,NORMAL0,COLOR0,TEXCOORD0 -> TEXCOORD0,FOG0,COLOR0,SV_POSITION0 |
| 0x58c0f0 | vs | 5824 | 99 | 0 | 0 | 29 | 3D (WorldView x ViewProjection); 6 POSITION streams (vertex-blend: positions pre-transformed per influence); 2 BLENDINDICES inputs (dynamic index into cbShaderParam, likely the LocalProjection/LightMatrix palette); KICK input (name suggests the PS2 GS ADC/kick flag; use not analysed); outputs FOG | POSITION0,POSITION1,POSITION2,POSITION3,POSITION4,POSITION5,TEXCOORD0,BLENDINDICES0,BLENDINDICES1,COLOR0,KICK0 -> TEXCOORD0,FOG0,COLOR0,SV_POSITION0 |
| 0x58d7b0 | vs | 3084 | 39 | 0 | 0 | 28 | 3D (WorldView x ViewProjection); KICK input (name suggests the PS2 GS ADC/kick flag; use not analysed); outputs FOG | POSITION0,COLOR0,TEXCOORD0,KICK0 -> TEXCOORD0,FOG0,COLOR0,SV_POSITION0 |
| 0x58e3c0 | vs | 2464 | 21 | 0 | 0 | 18 | 3D (WorldView x ViewProjection); 1 BLENDINDICES inputs (dynamic index into cbShaderParam, likely the LocalProjection/LightMatrix palette); lit (NORMAL); outputs FOG | NORMAL0,TEXCOORD0,POSITION0,BLENDINDICES0 -> TEXCOORD0,FOG0,COLOR0,SV_POSITION0 |
| 0x58ed60 | vs | 2280 | 19 | 0 | 0 | 20 | 3D (WorldView x ViewProjection); 1 BLENDINDICES inputs (dynamic index into cbShaderParam, likely the LocalProjection/LightMatrix palette) | POSITION0,BLENDINDICES0 -> COLOR0,SV_POSITION0 |
| 0x58f650 | vs | 4032 | 55 | 0 | 0 | 19 | 3D (WorldView x ViewProjection); 6 BLENDWEIGHT inputs; 2 BLENDINDICES inputs (dynamic index into cbShaderParam, likely the LocalProjection/LightMatrix palette); lit (NORMAL); outputs FOG | NORMAL0,TEXCOORD0,POSITION0,BLENDINDICES0,BLENDINDICES1,BLENDWEIGHT0,BLENDWEIGHT1,BLENDWEIGHT2,BLENDWEIGHT3,BLENDWEIGHT4,BLENDWEIGHT5 -> TEXCOORD0,FOG0,COLOR0,SV_POSITION0 |
| 0x590610 | vs | 4028 | 67 | 0 | 0 | 11,12 | 3D (WorldView x ViewProjection); 1 BLENDINDICES inputs (dynamic index into cbShaderParam, likely the LocalProjection/LightMatrix palette); lit (NORMAL); outputs FOG | NORMAL0,TEXCOORD0,POSITION0,COLOR0,BLENDINDICES0 -> TEXCOORD0,FOG0,COLOR0,SV_POSITION0 |
| 0x5915d0 | vs | 5924 | 107 | 0 | 0 | 15,16 | 3D (WorldView x ViewProjection); 7 POSITION streams (vertex-blend: positions pre-transformed per influence); 2 BLENDINDICES inputs (dynamic index into cbShaderParam, likely the LocalProjection/LightMatrix palette); lit (NORMAL); outputs FOG | POSITION0,POSITION1,POSITION2,POSITION3,POSITION4,POSITION5,POSITION6,TEXCOORD0,BLENDINDICES0,BLENDINDICES1,COLOR0,NORMAL0,NORMALINDEX0 -> TEXCOORD0,FOG0,COLOR0,SV_POSITION0 |
| 0x592d00 | vs | 3592 | 45 | 0 | 0 | 30 | 3D (WorldView x ViewProjection); 6 POSITION streams (vertex-blend: positions pre-transformed per influence); 2 BLENDINDICES inputs (dynamic index into cbShaderParam, likely the LocalProjection/LightMatrix palette) | POSITION0,POSITION1,POSITION2,POSITION3,POSITION4,POSITION5,BLENDINDICES0,BLENDINDICES1 -> COLOR0,SV_POSITION0 |
| 0x593b10 | vs | 2880 | 36 | 0 | 0 | 10 | screen-space / GS pixel-coordinate path (no WorldView; xyOffset/MagicScale/Projection2D or SafeScale); 6 POSITION streams (vertex-blend: positions pre-transformed per influence); 2 BLENDINDICES inputs (dynamic index into cbShaderParam, likely the LocalProjection/LightMatrix palette); KICK input (name suggests the PS2 GS ADC/kick flag; use not analysed) | POSITION0,POSITION1,POSITION2,POSITION3,POSITION4,POSITION5,TEXCOORD0,BLENDINDICES0,BLENDINDICES1,COLOR0,KICK0 -> TEXCOORD0,COLOR0,SV_POSITION0 |
| 0x594650 | vs | 2900 | 34 | 0 | 0 | 31,35 | 3D (WorldView x ViewProjection); outputs FOG | POSITION0,COLOR0,TEXCOORD0 -> TEXCOORD0,FOG0,COLOR0,SV_POSITION0 |
| 0x5951b0 | vs | 4660 | 74 | 0 | 0 | 32,36 | 3D (WorldView x ViewProjection); 8 BLENDWEIGHT inputs; outputs FOG | POSITION0,COLOR0,BLENDWEIGHT0,BLENDWEIGHT1,BLENDWEIGHT2,BLENDWEIGHT3,BLENDWEIGHT4,BLENDWEIGHT5,BLENDWEIGHT6,BLENDWEIGHT7,TEXCOORD0 -> TEXCOORD0,FOG0,COLOR0,SV_POSITION0 |
| 0x5963f0 | vs | 4004 | 66 | 0 | 0 | 33,37 | 3D (WorldView x ViewProjection); lit (NORMAL); outputs FOG | POSITION0,NORMAL0,COLOR0,TEXCOORD0 -> TEXCOORD0,FOG0,COLOR0,SV_POSITION0 |
| 0x5973a0 | vs | 5160 | 91 | 0 | 0 | 34,38 | 3D (WorldView x ViewProjection); 8 BLENDWEIGHT inputs; lit (NORMAL); outputs FOG | POSITION0,NORMAL0,COLOR0,BLENDWEIGHT0,BLENDWEIGHT1,BLENDWEIGHT2,BLENDWEIGHT3,BLENDWEIGHT4,BLENDWEIGHT5,BLENDWEIGHT6,BLENDWEIGHT7,TEXCOORD0 -> TEXCOORD0,FOG0,COLOR0,SV_POSITION0 |
| 0x5987d0 | vs | 3684 | 58 | 0 | 0 | 39,41 | 3D (WorldView x ViewProjection); outputs FOG | POSITION0,COLOR0,TEXCOORD0 -> TEXCOORD0,FOG0,COLOR0,SV_POSITION0 |
| 0x599640 | vs | 5632 | 107 | 0 | 0 | 40,42 | 3D (WorldView x ViewProjection); 4 BLENDWEIGHT inputs; 1 BLENDINDICES inputs (dynamic index into cbShaderParam, likely the LocalProjection/LightMatrix palette); lit (NORMAL); outputs FOG | POSITION0,NORMAL0,COLOR0,BLENDWEIGHT0,BLENDWEIGHT1,BLENDWEIGHT2,BLENDWEIGHT3,BLENDINDICES0,TEXCOORD0 -> TEXCOORD0,FOG0,COLOR0,SV_POSITION0 |
| 0x59ac40 | vs | 796 | 12 | 0 | 0 | 45,46,47 | [verification] full-screen QUAD generated from SV_VertexID as a 4-vertex TRIANGLESTRIP (no vertex buffer): id 0..3 -> pos (-1,1),(1,1),(-1,-1),(1,-1), uv (0,0),(1,0),(0,1),(1,1); color=0. A 3-vertex draw covers only the upper-left half (see note under section 4). Was: 'fullscreen triangle ... uv(0..2)' | SV_VertexID0 -> TEXCOORD0,COLOR0,SV_POSITION0 |
| 0x59af60 | ps | 1856 | 30 | 4 | 1 | 3,4,8 | PSFLAGS bit24 -> alpha := extTexAlpha; alpha test via PSFLAGS/alphaRef | TEXCOORD0,COLOR0,SV_POSITION0 -> SV_TARGET0 |
| 0x59b6a0 | ps | 1916 | 32 | 4 | 1 | 5 | PSFLAGS bit24 -> alpha := extTexAlpha; alpha test via PSFLAGS/alphaRef | TEXCOORD0,COLOR0,SV_POSITION0 -> SV_TARGET0 |
| 0x59be20 | ps | 1876 | 31 | 4 | 1 | 22 | PSFLAGS bit24 -> alpha := extTexAlpha; alpha test via PSFLAGS/alphaRef | TEXCOORD0,COLOR0,SV_POSITION0 -> SV_TARGET0 |
| 0x59c580 | ps | 1896 | 31 | 4 | 1 | 9 | PSFLAGS bit24 -> alpha := extTexAlpha; alpha test via PSFLAGS/alphaRef | TEXCOORD0,COLOR0,SV_POSITION0 -> SV_TARGET0 |
| 0x59ccf0 | ps | 1584 | 27 | 4 | 0 | 2 | untextured: vertex colour * alpha test; alpha test via PSFLAGS/alphaRef | COLOR0,SV_POSITION0 -> SV_TARGET0 |
| 0x59d320 | ps | 4052 | 93 | 4 | 4 | 6 | manual bilinear with GS-style REGION clamp/repeat (RegionU/V), 4 taps; PSFLAGS bit24 -> alpha := extTexAlpha; alpha test via PSFLAGS/alphaRef | TEXCOORD0,COLOR0,SV_POSITION0 -> SV_TARGET0 |
| 0x59e300 | ps | 3108 | 71 | 4 | 12 | 7 | manual bilinear with GS-style REGION clamp/repeat (RegionU/V), 12 taps; PSFLAGS bit24 -> alpha := extTexAlpha; alpha test via PSFLAGS/alphaRef | TEXCOORD0,COLOR0,SV_POSITION0 -> SV_TARGET0 |
| 0x59ef30 | ps | 2008 | 34 | 4 | 1 |  | fog: lerp(FogColor, colour, fogFactor); alpha test via PSFLAGS/alphaRef | TEXCOORD0,FOG0,COLOR0,SV_POSITION0 -> SV_TARGET0 |
| 0x59f710 | ps | 2048 | 35 | 4 | 1 | 12,14,16,26,44 | fog: lerp(FogColor, colour, fogFactor); alpha test via PSFLAGS/alphaRef | TEXCOORD0,FOG0,COLOR0,SV_POSITION0 -> SV_TARGET0 |
| 0x59ff10 | ps | 1832 | 29 | 4 | 1 |  | alpha test via PSFLAGS/alphaRef | TEXCOORD0,FOG0,COLOR0,SV_POSITION0 -> SV_TARGET0 |
| 0x5a0640 | ps | 2084 | 37 | 5 | 1 | 24 | fog: lerp(FogColor, colour, fogFactor); alpha test via PSFLAGS/alphaRef; extra discard path (5 discards) | TEXCOORD0,FOG0,COLOR0,SV_POSITION0 -> SV_TARGET0 |
| 0x5a0e70 | ps | 1832 | 29 | 4 | 1 |  | alpha test via PSFLAGS/alphaRef | TEXCOORD0,FOG0,COLOR0,SV_POSITION0 -> SV_TARGET0 |
| 0x5a15a0 | ps | 2076 | 37 | 5 | 1 | 25 | fog: lerp(FogColor, colour, fogFactor); alpha test via PSFLAGS/alphaRef; extra discard path (5 discards) | TEXCOORD0,FOG0,COLOR0,SV_POSITION0 -> SV_TARGET0 |
| 0x5a1dc0 | ps | 2008 | 34 | 4 | 1 | 11,13,15,17,18,19,21,23,27,28,29,43 | fog: lerp(FogColor, colour, fogFactor); alpha test via PSFLAGS/alphaRef | TEXCOORD0,FOG0,COLOR0,SV_POSITION0 -> SV_TARGET0 |
| 0x5a25a0 | ps | 1804 | 29 | 4 | 1 | 10 | alpha test via PSFLAGS/alphaRef | TEXCOORD0,COLOR0,SV_POSITION0 -> SV_TARGET0 |
| 0x5a2cb0 | ps | 452 | 2 | 0 | 0 | 20,30 | passthrough vertex colour | COLOR0,SV_POSITION0 -> SV_TARGET0 |
| 0x5a2e80 | ps | 2060 | 35 | 4 | 1 | 31,32,33,34,39,40 | fog: lerp(FogColor, colour, fogFactor); PSFLAGS bit24 -> alpha := extTexAlpha; alpha test via PSFLAGS/alphaRef | TEXCOORD0,FOG0,COLOR0,SV_POSITION0 -> SV_TARGET0 |
| 0x5a3690 | ps | 2224 | 38 | 4 | 2 | 35,36,37,38,41,42 | two textures t0/t1 lerp by multiTextureAlpha; fog: lerp(FogColor, colour, fogFactor); PSFLAGS bit24 -> alpha := extTexAlpha; alpha test via PSFLAGS/alphaRef | TEXCOORD0,FOG0,COLOR0,SV_POSITION0 -> SV_TARGET0 |
| 0x5a3f40 | ps | 612 | 2 | 0 | 1 | 46 | plain copy/blit: sample t0 at uv | TEXCOORD0,COLOR0,SV_POSITION0 -> SV_TARGET0 |
| 0x5a41b0 | ps | 924 | 9 | 0 | 2 | 45 | NV12 -> RGB (BT.709 limited range): t0=Y (R8), t1=UV (R8G8); alpha=1 | TEXCOORD0,COLOR0,SV_POSITION0 -> SV_TARGET0 |
| 0x5a4550 | ps | 9988 | 360 | 0 | 1 | 47 | final composition: inverse-gamma + colour-vision-deficiency simulation (360 instr) | TEXCOORD0,COLOR0,SV_POSITION0 -> SV_TARGET0 |

Constant-buffer members actually referenced, per shader, are in `shaders.json` (`cbuffers_used`).

## 4. Shader pairs: input layouts, strides, which pass

Extracted by running `0x11e1d0` (the shader-table initialiser, a straight-line function) in Unicorn against a fake device object with `D3DStripShader` hooked (`tools/run_shaderinit.py`; no game state, no I/O). Record layout in `GXDeviceImpl` (+0x7bf8 + idx*0x228): +0x00 VS blob, +0x08 PS blob (taken from the 21-entry PS table at +0x7b48 by the caller's 5th argument), +0x10 vertex stride, +0x14 / +0x18 two small ints (6th/7th args; values 0-2 and 1-2; meaning not determined), +0x20 input elements (D3D12_INPUT_ELEMENT_DESC, 32 B each), +0x220 element count. 48 slots, indices 0 and 1 are never populated.

| Pair | VS | PS (T1 idx) | Stride | Layout (SEM idx:DXGI@slot+offset) | Extra args | Used by (setShader immediates, call-site fn) |
|---|---|---|---|---|---|---|
| 2 | 0x586c80 | 0x59ccf0 (4) | 20 | POSITION0:f3@0 COLOR0:u8x4@0 | 2,1 | 0x14e050, 0x18a130, 0x18a2d0, 0x1c63b0, 0x1c6b00, 0x20ef60 +14 |
| 3 | 0x586160 | 0x59af60 (0) | 28 | POSITION0:f3@0 COLOR0:u8x4@0 TEXCOORD0:f2@0 | 2,1 | 0x14d8c0, 0x15fee0, 0x16ca50, 0x16cc60, 0x1969a0, 0x1ae760 +8 |
| 4 | 0x586710 | 0x59af60 (0) | 28 | POSITION0:f3@0 COLOR0:u8x4@0 TEXCOORD0:f2@0 | 2,1 | 0x168c90, 0x169bd0, 0x1c6520, 0x24c7b0 |
| 5 | 0x586160 | 0x59b6a0 (1) | 28 | POSITION0:f3@0 COLOR0:u8x4@0 TEXCOORD0:f2@0 | 2,1 | dynamic / not statically resolved |
| 6 | 0x586160 | 0x59d320 (5) | 28 | POSITION0:f3@0 COLOR0:u8x4@0 TEXCOORD0:f2@0 | 2,1 | 0x197f90 |
| 7 | 0x586160 | 0x59e300 (6) | 28 | POSITION0:f3@0 COLOR0:u8x4@0 TEXCOORD0:f2@0 | 2,1 | 0x1c67c0 |
| 8 | 0x5871b0 | 0x59af60 (0) | 32 | POSITION0:f4@0 TEXCOORD0:f2@0 COLOR0:u8x4@0 | 0,1 | 0x24b2f0, 0x24b480, 0x24df90, 0x2e1db0, 0x2e2360 |
| 9 | 0x5871b0 | 0x59c580 (3) | 32 | POSITION0:f4@0 TEXCOORD0:f2@0 COLOR0:u8x4@0 | 0,1 | dynamic / not statically resolved |
| 10 | 0x593b10 | 0x5a25a0 (15) | 120 | POSITION0:f4@0 POSITION1:f4@0 POSITION2:f4@0 POSITION3:f4@0 POSITION4:f4@0 POSITION5:f4@0 TEXCOORD0:f2@0 BLENDINDICES0:DXGI_30@0 BLENDINDICES1:DXGI_30@0 COLOR0:u8x4@0 KICK0:u32@0 | 0,1 | dynamic / not statically resolved |
| 11 | 0x590610 | 0x5a1dc0 (13) | 40 | NORMAL0:f3@0 TEXCOORD0:f2@0 POSITION0:f3@0 COLOR0:u8x4@0 BLENDINDICES0:DXGI_36@0 | 0,1 | 0x1a0d10, 0x1bde60 |
| 12 | 0x590610 | 0x59f710 (8) | 40 | NORMAL0:f3@0 TEXCOORD0:f2@0 POSITION0:f3@0 COLOR0:u8x4@0 BLENDINDICES0:DXGI_36@0 | 0,1 | dynamic / not statically resolved |
| 13 | 0x58b530 | 0x5a1dc0 (13) | 40 | POSITION0:f3@0 NORMAL0:f3@0 COLOR0:u8x4@0 TEXCOORD0:f2@0 | 1,1 | 0x1a0d10 |
| 14 | 0x58b530 | 0x59f710 (8) | 40 | POSITION0:f3@0 NORMAL0:f3@0 COLOR0:u8x4@0 TEXCOORD0:f2@0 | 1,1 | dynamic / not statically resolved |
| 15 | 0x5915d0 | 0x5a1dc0 (13) | 152 | POSITION0:f4@0 POSITION1:f4@0 POSITION2:f4@0 POSITION3:f4@0 POSITION4:f4@0 POSITION5:f4@0 POSITION6:f4@0 TEXCOORD0:f2@0 BLENDINDICES0:DXGI_30@0 BLENDINDICES1:DXGI_30@0 COLOR0:u8x4@0 NORMAL0:f3@0 NORMALINDEX0:DXGI_43@0 | 0,1 | dynamic / not statically resolved |
| 16 | 0x5915d0 | 0x59f710 (8) | 152 | POSITION0:f4@0 POSITION1:f4@0 POSITION2:f4@0 POSITION3:f4@0 POSITION4:f4@0 POSITION5:f4@0 POSITION6:f4@0 TEXCOORD0:f2@0 BLENDINDICES0:DXGI_30@0 BLENDINDICES1:DXGI_30@0 COLOR0:u8x4@0 NORMAL0:f3@0 NORMALINDEX0:DXGI_43@0 | 0,1 | dynamic / not statically resolved |
| 17 | 0x58a980 | 0x5a1dc0 (13) | 40 | POSITION0:f3@0 NORMAL0:f3@0 COLOR0:u8x4@0 TEXCOORD0:f2@0 | 0,1 | dynamic / not statically resolved |
| 18 | 0x58e3c0 | 0x5a1dc0 (13) | 36 | NORMAL0:f3@0 TEXCOORD0:f2@0 POSITION0:f3@0 BLENDINDICES0:DXGI_36@0 | 0,1 | dynamic / not statically resolved |
| 19 | 0x58f650 | 0x5a1dc0 (13) | 68 | NORMAL0:f3@0 TEXCOORD0:f2@0 POSITION0:f3@0 BLENDINDICES0:DXGI_30@0 BLENDINDICES1:DXGI_30@0 BLENDWEIGHT0:DXGI_41@0 BLENDWEIGHT1:DXGI_41@0 BLENDWEIGHT2:DXGI_41@0 BLENDWEIGHT3:DXGI_41@0 BLENDWEIGHT4:DXGI_41@0 BLENDWEIGHT5:DXGI_41@0 | 0,1 | dynamic / not statically resolved |
| 20 | 0x58ed60 | 0x5a2cb0 (16) | 16 | POSITION0:f3@0 BLENDINDICES0:DXGI_36@0 | 0,1 | dynamic / not statically resolved |
| 21 | 0x589e70 | 0x5a1dc0 (13) | 28 | POSITION0:f3@0 COLOR0:u8x4@0 TEXCOORD0:f2@0 KICK0:u32@0 | 1,1 | dynamic / not statically resolved |
| 22 | 0x586160 | 0x59be20 (2) | 28 | POSITION0:f3@0 COLOR0:u8x4@0 TEXCOORD0:f2@0 | 2,1 | dynamic / not statically resolved |
| 23 | 0x588330 | 0x5a1dc0 (13) | 28 | POSITION0:f3@0 COLOR0:u8x4@0 TEXCOORD0:f2@0 | 0,1 | dynamic / not statically resolved |
| 24 | 0x588330 | 0x5a0640 (10) | 28 | POSITION0:f3@0 COLOR0:u8x4@0 TEXCOORD0:f2@0 | 0,1 | dynamic / not statically resolved |
| 25 | 0x588330 | 0x5a15a0 (12) | 28 | POSITION0:f3@0 COLOR0:u8x4@0 TEXCOORD0:f2@0 | 0,1 | dynamic / not statically resolved |
| 26 | 0x588330 | 0x59f710 (8) | 28 | POSITION0:f3@0 COLOR0:u8x4@0 TEXCOORD0:f2@0 | 0,1 | dynamic / not statically resolved |
| 27 | 0x589080 | 0x5a1dc0 (13) | 24 | POSITION0:f3@0 TEXCOORD0:f2@0 BLENDINDICES0:DXGI_36@0 | 0,1 | 0x4d7ed0 |
| 28 | 0x58d7b0 | 0x5a1dc0 (13) | 28 | POSITION0:f3@0 COLOR0:u8x4@0 TEXCOORD0:f2@0 KICK0:u32@0 | 1,1 | dynamic / not statically resolved |
| 29 | 0x58c0f0 | 0x5a1dc0 (13) | 120 | POSITION0:f4@0 POSITION1:f4@0 POSITION2:f4@0 POSITION3:f4@0 POSITION4:f4@0 POSITION5:f4@0 TEXCOORD0:f2@0 BLENDINDICES0:DXGI_30@0 BLENDINDICES1:DXGI_30@0 COLOR0:u8x4@0 KICK0:u32@0 | 0,1 | dynamic / not statically resolved |
| 30 | 0x592d00 | 0x5a2cb0 (16) | 108 | POSITION0:f4@0 POSITION1:f4@0 POSITION2:f4@0 POSITION3:f4@0 POSITION4:f4@0 POSITION5:f4@0 BLENDINDICES0:DXGI_30@0 BLENDINDICES1:DXGI_30@0 | 0,1 | dynamic / not statically resolved |
| 31 | 0x594650 | 0x5a2e80 (14) | 28 | POSITION0:f3@0 COLOR0:u8x4@0 TEXCOORD0:f2@0 | 1,1 | 0x4a6db0, 0x4a7770, 0x4a8150, 0x4a86b0, 0x4a8730, 0x4a8980 +5 |
| 32 | 0x5951b0 | 0x5a2e80 (14) | 60 | POSITION0:f3@0 COLOR0:u8x4@0 BLENDWEIGHT0:DXGI_41@0 BLENDWEIGHT1:DXGI_41@0 BLENDWEIGHT2:DXGI_41@0 BLENDWEIGHT3:DXGI_41@0 BLENDWEIGHT4:DXGI_41@0 BLENDWEIGHT5:DXGI_41@0 BLENDWEIGHT6:DXGI_41@0 BLENDWEIGHT7:DXGI_41@0 TEXCOORD0:f2@0 | 0,1 | dynamic / not statically resolved |
| 33 | 0x5963f0 | 0x5a2e80 (14) | 40 | POSITION0:f3@0 NORMAL0:f3@0 COLOR0:u8x4@0 TEXCOORD0:f2@0 | 1,1 | 0x4a78d0, 0x4a97d0 |
| 34 | 0x5973a0 | 0x5a2e80 (14) | 72 | POSITION0:f3@0 NORMAL0:f3@0 COLOR0:u8x4@0 BLENDWEIGHT0:DXGI_41@0 BLENDWEIGHT1:DXGI_41@0 BLENDWEIGHT2:DXGI_41@0 BLENDWEIGHT3:DXGI_41@0 BLENDWEIGHT4:DXGI_41@0 BLENDWEIGHT5:DXGI_41@0 BLENDWEIGHT6:DXGI_41@0 BLENDWEIGHT7:DXGI_41@0 TEXCOORD0:f2@0 | 0,1 | 0x4a97d0 |
| 35 | 0x594650 | 0x5a3690 (17) | 28 | POSITION0:f3@0 COLOR0:u8x4@0 TEXCOORD0:f2@0 | 1,2 | 0x4a97d0, 0x4ab3b0 |
| 36 | 0x5951b0 | 0x5a3690 (17) | 60 | POSITION0:f3@0 COLOR0:u8x4@0 BLENDWEIGHT0:DXGI_41@0 BLENDWEIGHT1:DXGI_41@0 BLENDWEIGHT2:DXGI_41@0 BLENDWEIGHT3:DXGI_41@0 BLENDWEIGHT4:DXGI_41@0 BLENDWEIGHT5:DXGI_41@0 BLENDWEIGHT6:DXGI_41@0 BLENDWEIGHT7:DXGI_41@0 TEXCOORD0:f2@0 | 0,2 | 0x4ab3b0 |
| 37 | 0x5963f0 | 0x5a3690 (17) | 40 | POSITION0:f3@0 NORMAL0:f3@0 COLOR0:u8x4@0 TEXCOORD0:f2@0 | 1,2 | dynamic / not statically resolved |
| 38 | 0x5973a0 | 0x5a3690 (17) | 72 | POSITION0:f3@0 NORMAL0:f3@0 COLOR0:u8x4@0 BLENDWEIGHT0:DXGI_41@0 BLENDWEIGHT1:DXGI_41@0 BLENDWEIGHT2:DXGI_41@0 BLENDWEIGHT3:DXGI_41@0 BLENDWEIGHT4:DXGI_41@0 BLENDWEIGHT5:DXGI_41@0 BLENDWEIGHT6:DXGI_41@0 BLENDWEIGHT7:DXGI_41@0 TEXCOORD0:f2@0 | 0,2 | 0x4ab3b0 |
| 39 | 0x5987d0 | 0x5a2e80 (14) | 28 | POSITION0:f3@0 COLOR0:u8x4@0 TEXCOORD0:f2@0 | 1,1 | dynamic / not statically resolved |
| 40 | 0x599640 | 0x5a2e80 (14) | 60 | POSITION0:f3@0 NORMAL0:f3@0 COLOR0:u8x4@0 BLENDWEIGHT0:DXGI_41@0 BLENDWEIGHT1:DXGI_41@0 BLENDWEIGHT2:DXGI_41@0 BLENDWEIGHT3:DXGI_41@0 BLENDINDICES0:DXGI_30@0 TEXCOORD0:f2@0 | 0,1 | dynamic / not statically resolved |
| 41 | 0x5987d0 | 0x5a3690 (17) | 28 | POSITION0:f3@0 COLOR0:u8x4@0 TEXCOORD0:f2@0 | 1,2 | dynamic / not statically resolved |
| 42 | 0x599640 | 0x5a3690 (17) | 60 | POSITION0:f3@0 NORMAL0:f3@0 COLOR0:u8x4@0 BLENDWEIGHT0:DXGI_41@0 BLENDWEIGHT1:DXGI_41@0 BLENDWEIGHT2:DXGI_41@0 BLENDWEIGHT3:DXGI_41@0 BLENDINDICES0:DXGI_30@0 TEXCOORD0:f2@0 | 0,2 | dynamic / not statically resolved |
| 43 | 0x5877a0 | 0x5a1dc0 (13) | 60 | POSITION0:f4@0 POSITION1:f4@0 COLOR0:f4@0 TEXCOORD0:f2@0 | 0,1 | dynamic / not statically resolved |
| 44 | 0x5877a0 | 0x59f710 (8) | 60 | POSITION0:f4@0 POSITION1:f4@0 COLOR0:f4@0 TEXCOORD0:f2@0 | 0,1 | dynamic / not statically resolved |
| 45 | 0x59ac40 | 0x5a41b0 (19) | 0 | (none: SV_VertexID) | 2,2 | fixed-function pass (0x10c7e0 / 0x10dbc0) |
| 46 | 0x59ac40 | 0x5a3f40 (18) | 0 | (none: SV_VertexID) | 2,1 | fixed-function pass (0x10c7e0 / 0x10dbc0) |
| 47 | 0x59ac40 | 0x5a4550 (20) | 0 | (none: SV_VertexID) | 2,1 | fixed-function pass (0x10c7e0 / 0x10dbc0) |

verification NOTE (verified in asm of 0x59ac40 and disasm of 0x10c7e0 / 0x10dbc0): the VS emits a 4-vertex quad strip. 0x10c7e0 draws DrawInstanced(4,1,0,0) after IASetPrimitiveTopology(5 = TRIANGLESTRIP), a full quad. 0x10dbc0 (fixed pair 46, copy blit) draws DrawInstanced(3,1,0,0) with the same strip topology, which rasterises only the triangle (-1,1),(1,1),(-1,-1), i.e. the upper-left half of the viewport (the builder text called this a fullscreen triangle: that is incorrect). Whether the viewport/scissor set in 0x10dbc0 makes the half-triangle intentional is not determined.

verification NOTE (pair record +0x14, 'u14'): fn 0x1121d0 at 0x112d12-0x112d27 reads pair record +0x14 (`[rec+0x7c0c]`), computes `(~(u14>>1))&1` and ORs it into the sampler-heap index used for root param 4. Hence u14 bit 1 selects the sampler filter: u14=2 (2D UI pairs 2-7,22 and fullscreen pairs 45-47) = linear, u14=0/1 (3D pairs) = the other filter of the pair (anisotropic x4 per the 0x1247b2 creation loop). Derived statically, not run.

verification NOTE (unreferenced PS): PS 0x59ef30, 0x59ff10, 0x5a0e70 (PS-table indices 7, 9, 11) are referenced by no registered pair in the Unicorn dump. Static check: pair registration is inlined at ~17 sites in 0x11e1d0 (0x120895-0x120c87) and the sampled sites agree with the dump; the three blobs are unused by this run, but the run is of the unconditional straight-line path only, so 'never used' is unproven.

Pairs 45, 46, 47 have no vertex input (VS 0x59ac40 uses SV_VertexID): 45 = NV12 movie blit (PS 0x5a41b0), 46 = copy blit (PS 0x5a3f40; hard-coded by `0x10dbc0`), 47 = final composition (PS 0x5a4550: inverse-gamma + colour-vision simulation). PSO key class for these: `dw2 = 0x7e00`, cull NONE, no blend flag (see PSO-KEY.md).

Static `setShader` call-site analysis (`tools/setter_args.py`, 122 call sites of `0x127030`): immediates seen {2,3,4,6,7,8,11,13,27,31,33,34,35,36,38}; 34 sites pass a computed value and 11 could not be resolved by the backward slice. The "Used by" column lists the calling functions (addresses only; the callers are game classes whose names are not recovered). The attribution of render passes is therefore partial; callers are unnamed game functions (one known hint: `0x20f630`, RTTI `gb::GSSETDRAWPACKET`, builds GS GIF tags and calls GX `0x111970`; `gb::` = gummi ship classes).

## 5. DXBC to MSL: options, recommendation, proof

Requirement: translate 47 small, offline-fixed SM4.0 shaders (largest 360 instructions; no compute/GS/HS/DS/UAV, only `sample`, integer bit ops, `discard`, `round_ni/pi`, dynamic cb indexing). Conversion can run offline on Linux or build-host.

| Option | What it does | Licence | Risk / fit |
|---|---|---|---|
| A. dxbc-spirv (doitsujin, MIT) + SPIRV-Cross (Apache-2.0) | DXBC (SM <= 5.1) -> SPIR-V 1.6 (Vulkan memory model) -> MSL | MIT / Apache-2.0 | SM4 is in scope; actively maintained (1,096 commits; HEAD cloned here is dated 2026-11-14). Output SPIR-V is Vulkan-flavoured (README warns it is "likely not useful as-is" from the standalone tool; apps write their own resource lowering): we must map `cb2/cb3/cb4/cb6`, `t0/t1`, `s0` to Metal buffer/texture/sampler indices ourselves and run SPIRV-Cross with `--msl-argument-buffers` off (fixed small model). SPIR-V 1.6 may need `spirv-opt`/downgrade before older SPIRV-Cross accepts it. Library API is C++ (needs Meson, builds in the same tree). **Recommended** if hand translation is not chosen. |
| B. HLSLcc (Unity, MIT) | DXBC -> GLSL / Metal directly | MIT (+BSD bstring) | Proven in shipping engines, direct Metal backend. Last-commit date not visible (32 commits on master; unmaintained since ~2019 as far as known, unverified). Metal output targets older MSL; SM4.0 fine. Good fallback; needs a C++11 build. |
| C. vkd3d-shader (Wine, LGPL-2.1) | TPF/DXBC -> SPIR-V, experimental MSL target | LGPL-2.1 | MSL backend is experimental; per release notes 1.16/1.17 the DXIL->MSL path gained sampling, loops, integer ops; unverified whether DXBC/TPF input reaches the MSL target. LGPL is fine for an offline build tool but adds licence handling. Not recommended as first choice. |
| D. Apple Metal Shader Converter | DXIL (SM6) -> metallib | Apple terms (not stated on the page) | Takes DXIL only (page lists SM6.0-6.6). Our blobs are DXBC with no HLSL source, so unusable without a DXBC->DXIL step. Rejected. |
| E. Hand translation | 47 shaders by hand to MSL, driven by the asm in `shaders/asm/` | own work | Feasible: 47 files, 32 of them under 40 instructions, 44 under 100; the large ones are the 360-instruction final composition and the ~100-instruction skinning VS. Highest fidelity control and zero tool risk; cost is human review time and no automated equivalence check. Because the shaders are a closed, fixed set and the 3D VS family are variants of the same code, hand translation of the PS (21) and a macro-built VS family is realistic. |

**Recommendation.** Use A (dxbc-spirv + SPIRV-Cross) as the production converter, validated by hand-translating the 3 representative shaders below and by golden-image comparison (owner-gated), with E as fallback for any shader the converter mis-handles (candidates: the `round_ni/round_pi` pixel snapping in the 2D VS, `movc`-heavy PS 0x59d320/0x59e300, and the 360-instruction 0x5a4550). Licences are permissive (MIT, Apache-2.0).

**Proof status.** Built dxbc-spirv at `toolbuild/dxbc-spirv` (cloned from github.com/doitsujin/dxbc-spirv, HEAD 6196cc6, 2026-11-14): the library and `dxbc_disasm` compiled but linking `dxbc_compiler` failed because the SPIRV-Headers git submodule was not present; fetching the submodule was blocked by this session's permission classifier, so no automated DXBC->SPIR-V->MSL run was done. Fallback proof: three hand translations in `msl/`:
- `msl/fullscreen_vs_59ac40.metal` (VS, SV_VertexID quad, 4-vertex strip)
- `msl/nv12_ps_5a41b0.metal` (PS, NV12 -> RGB BT.709 limited range, constants copied from the DXBC)
- `msl/alphatest_fog_ps_5a1dc0.metal` (PS, alpha test via PSFLAGS + fog; the discard truth table was cross-checked against a literal python transcription of the DXBC: `tools/msl_reference.py`, 0 mismatches over 16 flag values x 25 alpha/ref pairs)
None of them has been compiled with a Metal compiler (no Metal toolchain on this host).
