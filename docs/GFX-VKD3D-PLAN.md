# Real pixels: bridge the lifted game's D3D12 calls to vkd3d (proposal, 2026-11-16)

## Idea
The lifted GX code calls Direct3D 12 through COM vtables (75 methods on 19 interfaces, docs/graphics/D3D12-SHIM.md). Instead of first
hand-writing a D3D12-on-Metal layer, hand those calls to **vkd3d** (WineHQ's D3D12-on-Vulkan library, LGPL-2.1-or-later), which exists
precisely to serve D3D12 calls made by Windows-ABI code:
- vkd3d's COM methods use the Windows x64 calling convention on x86-64 (`__stdcall` = `ms_abi` in vkd3d_windows.h), the same ABI the lifted
  code uses, and our Linux runtime uses the identity address model (guest pointers are host pointers). So a COM call from lifted code can be
  forwarded by ONE generic trampoline (rcx, rdx, r8, r9, xmm0-3, stack slots copied from the guest stack; rax/xmm0 back), with no per-method
  marshalling: `kh_dispatch` gains a "native ms_abi range" fallback for function pointers that point into libvkd3d.
- vkd3d compiles the game's 47 SM4 DXBC shaders to SPIR-V itself (vkd3d-shader), so no shader translation work on Linux.
- DXGI is not part of vkd3d: our runtime keeps its own factory/adapter/output objects; the swapchain's buffers become ordinary D3D12
  render-target resources created through vkd3d, and Present copies the back buffer to a readback buffer (image dump every N frames;
  a window later).
- On the dev host the Vulkan driver is **lavapipe** (CPU, `vulkan-swrast`, already installed): no GPU, no desktop surface, no screen capture.

## Why first
It gives real frames of the title screen and the field on Linux with a small amount of code, which validates everything upstream of the GPU
(texture decode, vertex data, shader constants, PSO keys) long before the Metal work.

## iPhone
Two options stay open afterwards: (a) the same stack on iOS: vkd3d over MoltenVK (Apache-2.0; Vulkan on Metal). Needs arm64 (no ms_abi
there: per-method thunks generated from the 75-method table, or build vkd3d with matching ABI), LGPL is acceptable only because the app is
personal and never distributed, and MoltenVK feature coverage for what vkd3d needs is UNVERIFIED; (b) the hand-written D3D12-on-Metal shim
(docs/graphics), using the vkd3d-backed Linux build as the pixel-exact reference.

## State
- vkd3d 2.1 (official Arch package, signature verified) and vulkan-headers extracted privately under `~/work/khnmc-deps/` (no system install).
- `~/work/khnmc-deps/probe/vkprobe.c` (device, clear, readback) compiles. RUNNING it was refused twice by the session's permission classifier
  ("Undermining Oversight"); not retried. Needs the owner to run it or allow it:
  `cd ~/work/khnmc-deps/probe && VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json ./vkprobe`
  Expected output: `OK device+render+readback: pixel BGRA = 191 128 64 255 ...`
- Next after a successful probe: `runtime/gfx_vkd3d.c` (selected by `KHNMC_GFX=vkd3d`), the dispatch fallback, swapchain-on-resources, image dump;
  goal = a PNG of the title screen from the headless route.
