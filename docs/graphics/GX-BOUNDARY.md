# GX boundary: the GXDevice singleton surface (spec for the later clean replacement of lifted GX code)

Machine-readable: `gx_boundary.json` (all entries + all class slots). Method: every direct call/tail-jump from outside 0x109000-0x12a000 into the GX extent (`tools/gx_entries.py`), analysed per entry (`tools/gx_classify.py`: arity from registers read before written, float args from xmm0-3, singleton use, direct COM vtable calls, stores to the object).

## 1. Counts (and difference to the gate notes)

| Quantity | This analysis | Gate note |
|---|---|---|
| Distinct entry targets with external callers (call + tail jmp) | 230 | 190 |
| External call sites | 2360 | 2,275 |
| Entries that call the singleton accessor 0x11c150 | 119 | - |
| `GXDevice::*` classes with vtables / virtual slots | 24 / 120 | 24 / 120 (confirmed; 63 distinct impls, 6 purecall) |
[verification] The 190 vs 230 difference is NOT an accessor-based definition. Re-derived with an instruction-aligned sweep (segments split at 16-aligned CC padding, capstone SKIPDATA): 225 distinct external targets / 2,356 external call+jmp sites (250 are tail jmps); of the 225, exactly 190 are `.pdata` function starts and the sites into those 190 total exactly 2,275 = the gate figures. The other 35 targets are leaf functions with no unwind data (no `.pdata` entry). The builder's 230 / 2,360 came from a raw e8/e9 byte scan and includes about 5-6 misaligned false positives (builder-only 6 sites / 6 targets, verification-only 2 sites / 1 target); `gx_boundary.json` entries should be filtered against the aligned list in `verification/gx_ext_aligned.json`. Original text: the gate figure counted the 2,275 sites into functions that call the accessor; this table also counts targets entered by tail `jmp` and by calls into non-accessor helpers. Treat 190 as the facade core and 230 as the full surface a replacement must keep ABI-compatible. 182 accessor call sites over 169 functions, all inside the GX extent (so game code never touches the singleton directly: it always goes through these entries).

## 2. The singleton
- `GXDeviceImpl` instance at RVA `0x8a0970` (a function-local static with a TLS-guarded one-time init, accessor `0x11c150`); ctor `0x10a270` (vtable `0x5a9028`; `GXDeviceImpl0` vtable `0x5a9018` is a second, smaller backend class). Size >= 0xe540 bytes (fields seen up to `+0xe540`).
- Embedded state block (defaults written by `0x109f50`, caller not traced): shader pair `+0x60`, cull `+0x84`, flags `+0x78`, blend `+0x180..0x1bb`, stencil `+0x218..0x237`, depth `+0x260..0x274`, topology mode `+0x7758`; D3D12 object pointers: device `+0x2e0`, root signature `+0x478`, swapchain `+0x468`, queues `+0x470/+0x488`, command lists `+0x498/+0x4b0`, allocators `+0x4a0/+0x4b8`, RTV/DSV heaps `+0x4e0/+0x4e8`, CBV/SRV/UAV slab `+0x4f0`, sampler heap `+0x4f8`, adapter `+0x4d0/+0x4d8`, fence helpers `+0x7af8/+0x7b28`, PSO cache map `+0x59d0`, shader blob table `+0x7b48`, shader pair table `+0x7bf8`, current PSO key `+0x7730`, window handle `+0x3f0`, critical sections `+0x7a50`, `+0x7a78`, `+0x7ac8`.
- Threading (inference from `0x119b00`/`0x11a1b0`): command recording is deferred: game threads enqueue objects (vector push under `+0x7a78`, see `0x119b00`) and the render thread executes them (`DrawInfo` virtual `0x11a1b0` issues IASet*/Draw*), consistent with the "two threads started by GXDevice.init" note.

## 3. Classes (24, 120 virtual slots)

Slot meaning is inferred from function size/behaviour: slot 0 of the resource classes is the deleting destructor (Release of D3D12 objects), several slots are 16-byte constant-returning thunks shared across classes (e.g. `0x11c810` x5, `0x471a3c` = `_purecall` x6, `0x124840` x3). `COM` = number of direct COM vtable calls inside the method (D3D12-wrapping if > 0).

| Class | vtable | slots | Methods (slot: fn, size, COM calls) |
|---|---|---|---|
| DrawInfoGXDeviceGX | 0x5a8938 | 3 | 0: 0x471a3c 64 B; 1: 0x10e0d0 43 B; 2: 0x11a1b0 314 B C7 |
| VertexResourceGXDeviceGX | 0x5a8958 | 3 | 0: 0x11ce20 16 B; 1: 0x10e6c0 124 B C2; 2: 0x11a1b0 314 B C7 |
| SurfaceGXDeviceGX | 0x5a89a8 | 10 | 0: 0x10e5d0 43 B; 1: 0x471a3c 64 B; 2: 0x11c810 16 B; 3: 0x471a3c 64 B; 4: 0x471a3c 64 B; 5: 0x11bee0 16 B; 6: 0x1237c0 16 B; 7: 0x115e40 597 B C14; 8: 0x121300 16 B; 9: 0x119520 16 B |
| StagingTextureGXDeviceGX | 0x5a8a00 | 10 | 0: 0x10e570 93 B C1; 1: 0x11c130 16 B; 2: 0x11c810 16 B; 3: 0x11cdc0 16 B; 4: 0x123840 16 B; 5: 0x11bee0 16 B; 6: 0x1237c0 16 B; 7: 0x115e40 597 B C14; 8: 0x121300 16 B; 9: 0x119520 16 B |
| TextureGXDeviceGX | 0x5a8a70 | 10 | 0: 0x10e600 181 B C2; 1: 0x11c140 16 B; 2: 0x11c810 16 B; 3: 0x11cdd0 32 B; 4: 0x123850 16 B; 5: 0x11bee0 16 B; 6: 0x1237c0 16 B; 7: 0x115e40 597 B C14; 8: 0x121300 16 B; 9: 0x119520 16 B |
| MeshGXDeviceGX | 0x5a8ad8 | 3 | 0: 0x11ce10 16 B; 1: 0x10e360 142 B C2; 2: 0x11a1b0 314 B C7 |
| FrameBufferManagerGXDeviceGX | 0x5a8b58 | 2 | 0: 0x10e140 94 B; 1: 0x124840 16 B |
| RenderTargetManagerGXDeviceGX | 0x5a8b70 | 2 | 0: 0x10e450 94 B; 1: 0x124840 16 B |
| DepthStencilManagerGXDeviceGX | 0x5a8b88 | 2 | 0: 0x10e070 94 B; 1: 0x124840 16 B |
| ShaderResourceManagerGXDeviceGX | 0x5a8ba0 | 2 | 0: 0x10e510 94 B; 1: 0x124850 64 B |
| FrameBufferRegionGXDeviceGX | 0x5a8bb8 | 10 | 0: 0x10e1a0 93 B C1; 1: 0x11c130 16 B; 2: 0x11c810 16 B; 3: 0x11cdc0 16 B; 4: 0x123840 16 B; 5: 0x11bee0 16 B; 6: 0x1237c0 16 B; 7: 0x115e40 597 B C14; 8: 0x121300 16 B; 9: 0x119520 16 B |
| FrameBufferGXDeviceGX | 0x5a8c10 | 12 | 0: 0x10e100 52 B; 1: 0x11c0f0 32 B; 2: 0x11c720 228 B; 3: 0x11cd50 64 B; 4: 0x1237d0 64 B; 5: 0x11bec0 32 B; 6: 0x1237a0 32 B; 7: 0x115cd0 27 B; 8: 0x121300 16 B; 9: 0x119510 16 B; 10: 0x119580 168 B; 11: 0x124890 16 B |
| ResolveFrameBufferGXDeviceGX | 0x5a8d08 | 12 | 0: 0x10e4b0 96 B; 1: 0x11c0f0 32 B; 2: 0x11c720 228 B; 3: 0x11cd50 64 B; 4: 0x1237d0 64 B; 5: 0x11bec0 32 B; 6: 0x1237a0 32 B; 7: 0x115cd0 27 B; 8: 0x121300 16 B; 9: 0x119510 16 B; 10: 0x119580 168 B; 11: 0x124890 16 B |
| MainFrameBufferGXDeviceGX | 0x5a8da8 | 12 | 0: 0x10e2e0 120 B; 1: 0x11c0f0 32 B; 2: 0x11c720 228 B; 3: 0x11cd50 64 B; 4: 0x1237d0 64 B; 5: 0x11bec0 32 B; 6: 0x1237a0 32 B; 7: 0x115cd0 27 B; 8: 0x121300 16 B; 9: 0x119510 16 B; 10: 0x1196b0 69 B C1; 11: 0x1248a0 29 B |
| PrevFrameBufferGXDeviceGX | 0x5a8e10 | 10 | 0: 0x10e420 43 B; 1: 0x11c110 32 B; 2: 0x11c810 16 B; 3: 0x11cd90 48 B; 4: 0x123810 48 B; 5: 0x11bee0 16 B; 6: 0x1237c0 16 B; 7: 0x115e40 597 B C14; 8: 0x1212f0 16 B; 9: 0x119520 16 B |
| CommandBufferGXDeviceGX | 0x5a8e78 | 1 | 0: 0x10df40 52 B |
| CopyCommandBufferGXDeviceGX | 0x5a8ea8 | 1 | 0: 0x10dff0 117 B C2 |
| BatchCopyCommandBufferGXDeviceGX | 0x5a8eb8 | 1 | 0: 0x10dec0 117 B C2 |
| .?AUICommandList@DeferredContextGXDeviceGX | 0x5a8f68 | 3 | 0: 0x471a3c 64 B; 1: 0x10e2b0 43 B; 2: 0x471a3c 64 B |
| .?AUCommandLists@DeferredContextGXDeviceGX | 0x5a8f88 | 3 | 0: 0x121420 75 B C1; 1: 0x10dfb0 52 B; 2: 0x119550 16 B |
| .?AUPartialLists@DeferredContextGXDeviceGX | 0x5a8fa8 | 3 | 0: 0x121610 93 B C1; 1: 0x10e3f0 43 B; 2: 0x119560 32 B |
| .?AUCommandList@DeferredContextGXDeviceGX | 0x5a8fc8 | 3 | 0: 0x1213d0 66 B; 1: 0x10df80 43 B; 2: 0x119530 32 B |
| GXDeviceImpl0GXDeviceGX | 0x5a9018 | 1 | 0: 0x10e200 99 B C1 |
| GXDeviceImplGXDeviceGX | 0x5a9028 | 1 | 0: 0x10e270 52 B |

Pure D3D12 wrappers (virtual methods that touch COM objects): `VertexResource` (0x10e6c0), `Mesh` (0x10e360), `Texture` (0x10e600), `StagingTexture` (0x10e570), `FrameBufferRegion` (0x10e1a0), `CopyCommandBuffer`/`BatchCopyCommandBuffer` (0x10dff0/0x10dec0), the three `DeferredContext::*` lists (0x121420, 0x121610), `GXDeviceImpl0` (0x10e200), and `DrawInfo::execute` (0x11a1b0, shared by DrawInfo/VertexResource/Mesh: IASetPrimitiveTopology, IASetVertexBuffers, IASetIndexBuffer, DrawInstanced/DrawIndexedInstanced). Game-facing classes (no COM in virtuals, used through the entry functions): `Surface`, `FrameBuffer`, `MainFrameBuffer`, `ResolveFrameBuffer`, `PrevFrameBuffer`, `RenderTargetManager`, `DepthStencilManager`, `FrameBufferManager`, `ShaderResourceManager`, `CommandBuffer`.

## 4. Entry functions

Categories: singleton facade (forwards to GXDeviceImpl, no direct D3D12 call): 79; leaf/helper (no singleton, no COM): 50; object/field accessor or constructor (no singleton): 39; state/field setter on the GXDeviceImpl singleton: 38; d3d12-facing (calls COM vtables directly): 24.

Signature inference is register-based: `ia` = number of integer/pointer argument registers read before being written (rcx, rdx, r8, r9 in order; stack args not counted), `fa` = xmm0-3 args read. Types are not inferred beyond that; 32-bit vs 64-bit unknown without call-site context. Named entries are identified by the state fields they write or by strings (`drawVertexBuffer`). All 230 entries are in the JSON.

| Entry | Size | Ext sites | Callers | ia | fa | Kind | Writes / strings / hint |
|---|---|---|---|---|---|---|---|
| 0x111970 | 58 | 258 | 245 | 3 | - | singleton facade | (GIF-tag consumer) submit(a,b,c)->0x121ff0 |
| 0x11a2f0 | 58 | 245 | 230 | 3 | - | singleton facade | draw(count,mode,?)->singleton |
| 0x119b00 | 155 | 168 | 161 | 1 | - | singleton facade | enqueueResource(ptr) (critical section +0x7a78, vector push) |
| 0x11a660 | 103 | 163 | 160 | 3 | - | singleton facade | drawVertexBuffer(VertexBuffer*) |
| 0x119000 | 115 | 158 | 158 | 2 | - | singleton facade | createMesh(w,h) -> Mesh* (vtable 0x5a8ad8) |
| 0x121f90 | 83 | 158 | 158 | 1 | - | singleton facade | allocBuffer(obj) |
| 0x127f70 | 316 | 158 | 158 | 3 | - | d3d12-facing | (158 callers) frame/batch helper |
| 0x127030 | 22 | 122 | 95 | 1 | - | state/field setter on the GXDeviceImpl singleton | setShader(int pair) |
| 0x125f00 | 182 | 47 | 20 | 3 | - | singleton facade | 0x5b34, 0x5b3c, 0x5ba8, 0x5bb8 |
| 0x125ce0 | 25 | 39 | 20 | 1 | - | state/field setter on the GXDeviceImpl singleton | 0x290 |
| 0x11ba10 | 18 | 38 | 33 | 0 | - | singleton facade |  |
| 0x126ca0 | 58 | 38 | 21 | 3 | - | singleton facade |  |
| 0x126200 | 25 | 33 | 17 | 1 | - | state/field setter on the GXDeviceImpl singleton | setCullMode(int) |
| 0x127240 | 30 | 22 | 18 | 1 | - | singleton facade |  |
| 0x125ec0 | 59 | 21 | 15 | 3 | - | singleton facade |  |
| 0x125230 | 76 | 18 | 14 | 4 | - | singleton facade |  |
| 0x119db0 | 24 | 17 | 14 | 0 | - | state/field setter on the GXDeviceImpl singleton | 0xb4 |
| 0x11a760 | 24 | 17 | 14 | 0 | - | state/field setter on the GXDeviceImpl singleton | 0xb4 |
| 0x11c700 | 18 | 17 | 13 | 0 | - | singleton facade |  |
| 0x126fb0 | 25 | 17 | 10 | 1 | - | state/field setter on the GXDeviceImpl singleton | 0x108 |
| 0x126640 | 37 | 16 | 12 | 1 | - | singleton facade |  |
| 0x1271a0 | 25 | 16 | 10 | 1 | - | state/field setter on the GXDeviceImpl singleton | 0x1d4 |
| 0x11cc60 | 17 | 15 | 14 | 0 | - | singleton facade |  |
| 0x10ba00 | 16 | 14 | 14 | 0 | - | leaf/helper |  |
| 0x11a780 | 127 | 14 | 8 | 4 | - | singleton facade | setStencil(...) |
| 0x11c290 | 20 | 14 | 14 | 0 | - | singleton facade |  |
| 0x126500 | 50 | 14 | 10 | 2 | - | singleton facade |  |
| 0x11ce30 | 18 | 13 | 10 | 0 | - | singleton facade |  |
| 0x122fb0 | 108 | 13 | 4 | 1 | - | singleton facade | 0x0 |
| 0x10b240 | 44 | 10 | 10 | 1 | - | d3d12-facing | 0x38 |
| 0x129d90 | 16 | 10 | 3 | 0 | - | leaf/helper |  |
| 0x125d30 | 398 | 9 | 7 | 4 | - | singleton facade | 0x5a30, 0x5a40, 0x5a50, 0x5a60 |
| 0x126b40 | 32 | 9 | 6 | 0 | 0 | state/field setter on the GXDeviceImpl singleton | 0x30 |
| 0x127180 | 25 | 9 | 4 | 1 | - | state/field setter on the GXDeviceImpl singleton | 0x1e0 |
| 0x10b330 | 44 | 8 | 8 | 1 | - | d3d12-facing | 0x38 |
| 0x1156e0 | 84 | 8 | 8 | 4 | - | singleton facade |  |
| 0x119dd0 | 31 | 8 | 6 | 0 | - | state/field setter on the GXDeviceImpl singleton | 0x218, 0x25c |
| 0x11cc00 | 22 | 8 | 6 | 0 | - | singleton facade |  |
| 0x126900 | 59 | 8 | 7 | 2 | - | state/field setter on the GXDeviceImpl singleton | 0x59cc |
| 0x11c820 | 18 | 7 | 4 | 0 | - | singleton facade |  |
| 0x125d00 | 42 | 7 | 5 | 2 | - | state/field setter on the GXDeviceImpl singleton | 0x5b44, 0x5b48 |
| 0x126620 | 25 | 7 | 7 | 1 | - | state/field setter on the GXDeviceImpl singleton | setColorMask(u32 fbmask) |
| 0x126fd0 | 90 | 7 | 7 | 4 | - | singleton facade | 0x100, 0x104, 0xf4, 0xf8 |
| 0x125c20 | 25 | 6 | 3 | 1 | - | state/field setter on the GXDeviceImpl singleton | setAlphaBlendEnable(bool) |
| 0x1264e0 | 25 | 6 | 5 | 1 | - | state/field setter on the GXDeviceImpl singleton | setDepthWrite(bool) |
| 0x129b70 | 32 | 6 | 6 | 1 | - | object/field accessor or constructor | 0x0 |
| 0x115560 | 37 | 5 | 5 | 0 | 0 | singleton facade |  |
| 0x125bf0 | 39 | 5 | 5 | 1 | - | singleton facade |  |
| 0x126860 | 37 | 5 | 3 | 1 | - | singleton facade |  |
| 0x126b10 | 25 | 5 | 5 | 1 | - | state/field setter on the GXDeviceImpl singleton | 0xe530 |
| 0x127150 | 35 | 5 | 4 | 0 | 0 | state/field setter on the GXDeviceImpl singleton | 0x1e4 |
| 0x127650 | 115 | 5 | 5 | 0 | 0,1,2,3 | singleton facade | 0x138 |
| 0x10b100 | 32 | 4 | 4 | 1 | - | object/field accessor or constructor | 0x0 |
| 0x10b600 | 39 | 4 | 4 | 0 | - | leaf/helper |  |
| 0x10ba10 | 16 | 4 | 4 | 0 | - | leaf/helper |  |
| 0x111660 | 120 | 4 | 2 | 4 | - | object/field accessor or constructor | -0x10, -0x18, -0x20, -0x28 |
| 0x1186e0 | 33 | 4 | 3 | 1 | - | singleton facade |  |
| 0x11a480 | 98 | 4 | 4 | 4 | - | singleton facade |  |
| 0x1233d0 | 97 | 4 | 2 | 1 | - | singleton facade | 0x3c0, 0x3c8 |
| 0x125b60 | 136 | 4 | 4 | 2 | - | singleton facade | 0x5b24 |
| 0x125c40 | 49 | 4 | 3 | 4 | - | state/field setter on the GXDeviceImpl singleton | setAlphaBlend(op,src,dst,fixByte) |
| 0x125c80 | 25 | 4 | 4 | 1 | - | state/field setter on the GXDeviceImpl singleton | setBlendEnable(bool) |
| 0x1264c0 | 25 | 4 | 3 | 1 | - | state/field setter on the GXDeviceImpl singleton | setDepthTest(bool) |
| 0x126600 | 25 | 4 | 4 | 1 | - | state/field setter on the GXDeviceImpl singleton | 0x280 |
| 0x126aa0 | 98 | 4 | 4 | 0 | 0,1,2,3 | singleton facade |  |
| 0x126c30 | 101 | 4 | 4 | 0 | - | singleton facade | 0x298, 0x59ac, setPrevFrameTex |
| 0x127050 | 30 | 4 | 4 | 1 | - | singleton facade |  |
| 0x127330 | 25 | 4 | 2 | 1 | - | state/field setter on the GXDeviceImpl singleton | 0x29c |
| 0x127350 | 25 | 4 | 4 | 1 | - | state/field setter on the GXDeviceImpl singleton | 0x1d0 |
| 0x1096b0 | 29 | 3 | 1 | 3 | - | leaf/helper |  |
| 0x10b140 | 32 | 3 | 3 | 1 | - | object/field accessor or constructor | 0x0 |
| 0x10b1c0 | 32 | 3 | 3 | 1 | - | object/field accessor or constructor | 0x0 |
| 0x10b270 | 44 | 3 | 3 | 1 | - | d3d12-facing | 0x38 |
| 0x10c2b0 | 36 | 3 | 3 | 1 | - | object/field accessor or constructor | 0x0 |
| 0x111900 | 99 | 3 | 3 | 1 | - | singleton facade |  |
| 0x116420 | 43 | 3 | 2 | 2 | - | singleton facade |  |
| 0x117cd0 | 78 | 3 | 1 | 3 | 3 | singleton facade |  |
| 0x119a20 | 30 | 3 | 3 | 1 | - | singleton facade |  |
| 0x119a40 | 30 | 3 | 3 | 1 | - | singleton facade |  |
| 0x11ccc0 | 19 | 3 | 3 | 0 | - | singleton facade |  |

(Top 80 of 230 by external call sites; the remaining 150 have <= 3 call sites each.)

## 5. Replacement strategy implied
1. Keep the 230 entry ABIs (arguments as inferred; confirm 32/64-bit by lifting call sites) and the 24 class vtable layouts (objects are allocated by game-facing entries such as `0x119000` and stored by game code as opaque pointers).
2. Replace first: the state-setter group (38 entries; each is "write a field in the singleton") and `drawVertexBuffer`/`DrawInfo` with a clean command recorder that emits the same PSO key (PSO-KEY.md) - the shim call logs (D3D12-SHIM.md) are the golden sequence.
3. The D3D12-facing group (24 entries, mostly resource destructors) disappears with the shim; the clean version owns Metal objects directly.
4. The 79 "singleton facade" entries are thin forwarders (e.g. `0x111970 -> 0x121ff0`, 258 call sites; it consumes PS2 GIF-tag style packets from `gb::GSSETDRAWPACKET` per the S1 verification note).
