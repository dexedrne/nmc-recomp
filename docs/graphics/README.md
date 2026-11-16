# Batch A3: graphics spec (KHNMC: DECOMP EDITION, route B)

Binary: `pc/kh2fm.exe` (sha256 9002b2de6a1f91a7...). Everything here is derived from it, the owner's OpenKH data and Steam install (read-only), and public documentation. Nothing game-derived leaves this directory. No timing estimates anywhere.

| File | Content |
|---|---|
| `PSO-KEY.md`, `pso_key.py`, `pso_key_table.json` | exact 128-bit (96 effective) PSO key: bit table, desc mapping, producers, shader-pair table, verification |
| `SHADERS.md`, `shaders.json`, `shaders/*.dxbc`, `shaders/asm/*.asm`, `msl/*.metal` | 47 SM4 shaders: layouts, PSFLAGS/alphaRef/Region semantics, DXBC->MSL options, 3 hand-translated shaders |
| `D3D12-SHIM.md`, `d3d12_shim_methods.json`, `shim/d3d12_shim.hpp` | D3D12/DXGI call surface with arguments, Metal mapping, C++ interface spec |
| `FORMATS.md`, `formats.json` | texture / RT formats confirmed from exe and 37,757 DDS files |
| `MOVIES.md` | Media Foundation + D3D11 interop path; AVFoundation design; movie file facts |
| `GX-BOUNDARY.md`, `gx_boundary.json` | GXDevice singleton surface (230 entries, 24 classes / 120 slots) |
| `tools/` | analysis scripts (`emu.py` Unicorn harness for isolated functions, `verify_*.py`, `dxbc_dis.py` self-written SM4 disassembler, `comsites.py`, ...), `out/` raw dumps/disassembly, `venv/` (pefile, capstone, unicorn) |

## Corrections to earlier notes (carry forward)
1. Old PSO decode was wrong in several places; use `pso_key.py`. Key = 4 dwords, dw3 = 0; RTV format is always 0x57 and DSV 0x14; bit 21 is "depth-stencil present" and gates both depth and stencil; shader-pair table is `+0x7bf8`, 48 x 0x228 B, indices 2..47 populated.
2. DDS: BC1/2/3 + BGRA8 only; the four 16/24-bit formats are recognised but skipped (not converted). Data is 71% BC3 / 29% BGRA8; no BC7/BC6.
3. Shader count 47 (26 VS + 21 PS) confirmed; 21 PS blobs are held in a table at `+0x7b48`; 46 pairs registered.
4. Earlier S1 linear sweeps stopped at the first undecodable byte (~0x1121xx); GX scans must use SKIPDATA. Re-run counts: 622 indirect vtable calls in GX, 125 in the movie extent.
5. Queue vtbl 0x78 = Wait, 0x80/0x88 = timestamp calls.
6. GX entry surface 230 distinct targets / 2,360 external call sites by this method (gate: 190 / 2,275 by the accessor-based definition).

## Blocked / not done
- Automated DXBC->SPIR-V->MSL proof: `dxbc-spirv` built except `dxbc_compiler` (missing SPIRV-Headers submodule); the submodule fetch was denied by the permission classifier, so no converter run. Hand MSL for 3 shaders instead (uncompiled).
- Runtime-only facts (RT sizes, actual distinct PSO keys, movie codec/resolution) need owner-approved captures.

## verification corrections (independent re-derivation; originals of edited files in `verification/orig/`)
Re-verified without change: PSO key layout (consumer 0x117710, producers 0x1121d0 fragment / 0x10c7e0 / 0x10dbc0 disassembled by hand; only 3 call sites of 0x113590, 1 of 0x117710), 47 shader blobs byte-identical to a fresh scan, stage/instruction/discard/sample/resource data of all 47 vs own RDEF+SHDR parser, PSFLAGS alpha-test truth table, NV12 and alpha-test/fog hand-MSL against the asm, 622 / 205 / 125 / 51 vtable-call counts, API call-site addresses (CreateDXGIFactory1, D3D12CreateDevice, SerializeRootSignature, D3D11CreateDevice, MFCreateSourceReaderFromURL, 50 D3DStripShader), DDS loader format logic, 24 classes / 120 slots, 182 accessor sites / 169 functions, movie file sizes and header facts, external tool licences (dxbc-spirv MIT, SPIRV-Cross Apache-2.0, HLSLcc MIT, Metal Shader Converter DXIL-only, vkd3d MSL experimental, DXBC->MSL not documented).
Errors fixed:
1. Root-parameter order was wrong: param 4 = SAMPLER s0, param 5 = SRV t0, param 6 = SRV t1 (builder had 4 = t0, 5 = t1, 6 = s0). Fixed in SHADERS.md, D3D12-SHIM.md, d3d12_shim_methods.json, shim/d3d12_shim.hpp, shaders.json.
2. VS 0x59ac40 is a 4-vertex strip quad (uv 0..1), not a "fullscreen triangle"; 0x10dbc0's DrawInstanced(3) covers only half the viewport.
3. SetGraphicsRootConstantBufferView has 6 sites, not 5.
4. GX boundary: the 190 vs 230 explanation was wrong. 190 / 2,275 = targets that are .pdata function starts and the sites into them (reproduced exactly); aligned sweep gives 225 / 2,356; the builder's 230 / 2,360 contains ~6 byte-scan false positives.
5. Added: sampler filter selected by pair record +0x14 bit 1 (static); unreferenced PS blobs caveat.
Verified additional fact: the DDS census agrees with the loader predicate exactly (all 37,757 files satisfy the exact flag/mask tests 0x57 or DXT5; none are rejected or smaller than 4x4).
