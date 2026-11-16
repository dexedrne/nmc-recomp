// KHNMC:DECOMP EDITION - D3D12/DXGI shim interface spec (header-style), derived from kh2fm.exe (sha256 9002b2de...).
// Scope: exactly the methods the exe calls (see ../d3d12_shim_methods.json for call sites/evidence/confidence).
// All COM objects are plain C++ vtable layouts with the Windows x64 ABI preserved at the boundary: the lifted code
// calls `call [obj->vtbl + off]`, so every class below must expose a vtable whose slot OFFSETS equal the real D3D12/DXGI
// offsets listed in the comments (unused slots point at a function that traps). Methods returning structs
// (GetCPUDescriptorHandleForHeapStart, GetGPUDescriptorHandleForHeapStart, GetDesc) use the MSVC hidden-return-pointer
// convention (result pointer in RDX when `this` is in RCX), as the call sites in the exe show.
#pragma once
#include <cstdint>
namespace khnmc_shim {
using HRESULT = int32_t; constexpr HRESULT S_OK = 0, DXGI_ERROR_NOT_FOUND = (HRESULT)0x887a0002, DXGI_ERROR_DEVICE_REMOVED=(HRESULT)0x887a0005;
struct GUID { uint32_t a; uint16_t b, c; uint8_t d[8]; };
// ---- IIDs the exe passes (RVA of the GUID in kh2fm.exe in brackets)
// IDXGIFactory4 {1bc6ea02-ef36-464f-bf0c-21ca39e5168a}[0x5a88a8]  ID3D12Device {189819f1-1db6-4b57-be54-1821339b85f7}[0x5a88b8]
// IDXGIAdapter3 {645967a4-1392-4310-a798-8053ce3e93fd}[0x5a9558]  ID3D12CommandQueue {0ec870a6-5d7e-4c22-8cfc-5baae07616ed}[0x5a9030]
// ID3D12CommandAllocator {6102dee4-af59-4b09-b999-b44d73f09b24}[0x5a8e80]  ID3D12GraphicsCommandList {5b160d0f-ac1b-4185-8ba8-b3ae42a5a455}[0x5a8e90]
// ID3D12DescriptorHeap {8efb471d-616c-4f49-90f7-127bb763fa51}[0x5a8990]  ID3D12Fence {0a753dcf-c4d8-4b91-adf6-be5a60d95a76}[0x5a88e0]
// ID3D12Resource {696442be-a72e-4059-bc79-5b5c98040fad}[0x5a8900]  ID3D12Heap {6b3b2502-6e51-45b3-90ee-9884265e8df3}[0x5a88f0]
// ID3D12QueryHeap {0d9658ae-ed45-469e-a61d-970ec583cab4}[0x5a8e60]  ID3D12PipelineState {765a30f3-f624-4c6f-a828-ace948622445}[0x5a9408]
// IDXGIResource (movie) [0x5dbde0]  root-signature IID [0x5a90b0]  IDXGISwapChain3 QI target [0x491157 region]

// ---- free functions (imports resolved by the lifted loader): d3d12.dll#101, #115(Serialize), dxgi.dll CreateDXGIFactory1
extern "C" HRESULT D3D12CreateDevice(void* adapter, uint32_t minFeatureLevel /*0xb000*/, const GUID& iid, void** out);
extern "C" HRESULT D3D12SerializeRootSignature(const void* desc, uint32_t version /*1*/, void** blob, void** err);
extern "C" HRESULT CreateDXGIFactory1(const GUID& iid, void** out);

// ---- plain-data structs the call sites build (layouts = the public D3D12/DXGI structs; use d3d12.h definitions)
struct DESCRIPTOR_HEAP_DESC { uint32_t Type, NumDescriptors, Flags, NodeMask; };        // RTV(2)x320, DSV(3)x320, CBV_SRV_UAV(0)x16384|SHADER_VISIBLE(1), SAMPLER(1)x8|SHADER_VISIBLE
struct SWAP_CHAIN_DESC1 { uint32_t Width, Height, Format, Stereo, SampleCount, SampleQuality, BufferUsage, BufferCount, Scaling, SwapEffect, AlphaMode, Flags; }; // Format 0x57, Usage 0x30, Count 3, Effect 3, Flags 0x42
struct GRAPHICS_PIPELINE_STATE_DESC_Key { uint32_t dw[4]; };                            // see PSO-KEY.md; shim consumes the full 0x290-byte desc exactly as 0x117710 builds it

// ---- COM interfaces (offset = vtable byte offset)
struct IUnknown { virtual HRESULT QueryInterface(const GUID&, void**) = 0 /*0x00*/; virtual uint32_t AddRef() = 0 /*0x08*/; virtual uint32_t Release() = 0 /*0x10*/; };
struct IDXGIFactory4 : IUnknown { /* 0x40 */ HRESULT MakeWindowAssociation(void* hwnd, uint32_t flags);
  /* 0x60 (IDXGIFactory1) */ HRESULT EnumAdapters1(uint32_t i, struct IDXGIAdapter1** out);   // return NOT_FOUND after the first
  /* 0x78 (IDXGIFactory2) */ HRESULT CreateSwapChainForHwnd(void* queueAsDevice, void* hwnd, const SWAP_CHAIN_DESC1*, const void* fullscreenDesc /*NULL*/, void* restrictOutput /*NULL*/, struct IDXGISwapChain1** out); };
struct IDXGIAdapter1 : IUnknown { /* 0x38 */ HRESULT EnumOutputs(uint32_t i, struct IDXGIOutput** out); /* 0x50 */ HRESULT GetDesc1(void* DXGI_ADAPTER_DESC1 /*VendorId at +0x100*/);
  /* 0x70 (IDXGIAdapter3) */ HRESULT QueryVideoMemoryInfo(uint32_t node, uint32_t segGroup, void* info); };
struct IDXGIOutput : IUnknown { /* 0x38 */ HRESULT GetDesc(void* DXGI_OUTPUT_DESC); };
struct IDXGISwapChain3 : IUnknown {
  /* 0x48 */ HRESULT GetBuffer(uint32_t i, const GUID&, void** res);   /* 0x50 */ HRESULT SetFullscreenState(int, void*);   /* 0x60 */ HRESULT GetDesc(void*);
  /* 0x68 */ HRESULT ResizeBuffers(uint32_t count, uint32_t w, uint32_t h, uint32_t fmt, uint32_t flags);   /* 0x78 */ HRESULT GetContainingOutput(IDXGIOutput**);
  /* 0x98 */ HRESULT GetFullscreenDesc(void*);   /* 0xb0 */ HRESULT Present1(uint32_t sync, uint32_t flags, const void* params);
  /* 0xf8 */ HRESULT SetMaximumFrameLatency(uint32_t);   /* 0x108 */ void* GetFrameLatencyWaitableObject();   /* 0x120 */ uint32_t GetCurrentBackBufferIndex(); };
struct ID3D12DescriptorHeap : IUnknown { /* 0x48 */ void GetCPUDescriptorHandleForHeapStart(uint64_t* out); /* 0x50 */ void GetGPUDescriptorHandleForHeapStart(uint64_t* out); };
struct ID3D12Resource : IUnknown { /* 0x30 (ID3D12Object::SetName) */ HRESULT SetName(const wchar_t*); /* 0x40 */ HRESULT Map(uint32_t sub, const void* readRange, void** data); /* 0x58 */ uint64_t GetGPUVirtualAddress(); };
struct ID3D12Fence : IUnknown { /* 0x40 */ uint64_t GetCompletedValue(); /* 0x48 */ HRESULT SetEventOnCompletion(uint64_t v, void* event); /* 0x50 */ HRESULT Signal(uint64_t v); };
struct ID3D12CommandAllocator : IUnknown { /* 0x40 */ HRESULT Reset(); };
struct ID3D12CommandQueue : IUnknown { /* 0x50 */ void ExecuteCommandLists(uint32_t n, struct ID3D12GraphicsCommandList* const* lists);
  /* 0x70 */ HRESULT Signal(ID3D12Fence*, uint64_t); /* 0x78 */ HRESULT Wait(ID3D12Fence*, uint64_t); /* 0x80 */ HRESULT GetTimestampFrequency(uint64_t*); /* 0x88 */ HRESULT GetClockCalibration(uint64_t*, uint64_t*); };
struct ID3D12GraphicsCommandList : IUnknown {
  /* 0x48 */ HRESULT Close(); /* 0x50 */ HRESULT Reset(ID3D12CommandAllocator*, void* pso /*NULL*/);
  /* 0x60 */ void DrawInstanced(uint32_t vtx, uint32_t inst, uint32_t startV, uint32_t startI);   /* 0x68 */ void DrawIndexedInstanced(uint32_t idx, uint32_t inst, uint32_t startI, int32_t baseV, uint32_t startInst);
  /* 0x78 */ void CopyBufferRegion(...); /* 0x80 */ void CopyTextureRegion(...); /* 0x88 */ void CopyResource(ID3D12Resource* dst, ID3D12Resource* src);
  /* 0x98 */ void ResolveSubresource(ID3D12Resource* dst, uint32_t dsub, ID3D12Resource* src, uint32_t ssub, uint32_t fmt /*0x57*/);
  /* 0xa0 */ void IASetPrimitiveTopology(uint32_t d3dTopo /*1..5*/); /* 0xa8 */ void RSSetViewports(uint32_t n, const float* vp6); /* 0xb0 */ void RSSetScissorRects(uint32_t n, const int32_t* rect4);
  /* 0xb8 */ void OMSetBlendFactor(const float* rgba); /* 0xc0 */ void OMSetStencilRef(uint32_t); /* 0xc8 */ void SetPipelineState(struct ID3D12PipelineState*);
  /* 0xd0 */ void ResourceBarrier(uint32_t n, const void* barriers /*TRANSITION only, 0x20 B each*/);
  /* 0xe0 */ void SetDescriptorHeaps(uint32_t n, ID3D12DescriptorHeap* const*); /* 0xf0 */ void SetGraphicsRootSignature(void*);
  /* 0x100 */ void SetGraphicsRootDescriptorTable(uint32_t param /*4 = sampler s0, 5 = SRV t0, 6 = SRV t1 [verification]*/, uint64_t gpuHandle); /* 0x130 */ void SetGraphicsRootConstantBufferView(uint32_t param /*0..3*/, uint64_t gpuVA);
  /* 0x158 */ void IASetIndexBuffer(const void* view /*{loc,size,fmt=0x39 R16_UINT}*/); /* 0x160 */ void IASetVertexBuffers(uint32_t slot, uint32_t n, const void* views);
  /* 0x170 */ void OMSetRenderTargets(uint32_t n, const uint64_t* rtvs, int single, const uint64_t* dsv);
  /* 0x178 */ void ClearDepthStencilView(uint64_t dsv, uint32_t flags, float depth, uint8_t stencil, uint32_t nrects, const void* rects);
  /* 0x180 */ void ClearRenderTargetView(uint64_t rtv, const float* rgba, uint32_t nrects, const void* rects);
  /* 0x1a8 */ void EndQuery(void*, uint32_t, uint32_t); /* 0x1b0 */ void ResolveQueryData(...); };
struct ID3D12Device : IUnknown {
  /* 0x40 */ HRESULT CreateCommandQueue(const void* desc, const GUID&, void** out);   /* 0x48 */ HRESULT CreateCommandAllocator(uint32_t type, const GUID&, void** out);
  /* 0x50 */ HRESULT CreateGraphicsPipelineState(const void* desc0x290, const GUID&, void** out);
  /* 0x60 */ HRESULT CreateCommandList(uint32_t node, uint32_t type, ID3D12CommandAllocator*, void* pso, const GUID&, void** out);
  /* 0x68 */ HRESULT CheckFeatureSupport(uint32_t feature /*4*/, void* data, uint32_t size /*0x10*/);
  /* 0x70 */ HRESULT CreateDescriptorHeap(const DESCRIPTOR_HEAP_DESC*, const GUID&, void** out); /* 0x78 */ uint32_t GetDescriptorHandleIncrementSize(uint32_t type);
  /* 0x80 */ HRESULT CreateRootSignature(uint32_t node, const void* blob, size_t size, const GUID&, void** out);
  /* 0x90 */ void CreateShaderResourceView(ID3D12Resource*, const void* desc, uint64_t cpuHandle); /* 0xa0 */ void CreateRenderTargetView(ID3D12Resource*, const void* desc, uint64_t cpuHandle);
  /* 0xa8 */ void CreateDepthStencilView(ID3D12Resource*, const void* desc, uint64_t cpuHandle); /* 0xb0 */ void CreateSampler(const void* desc, uint64_t cpuHandle);
  /* 0xc0 */ void CopyDescriptorsSimple(uint32_t n, uint64_t dst, uint64_t src, uint32_t type);
  /* 0xd8 */ HRESULT CreateCommittedResource(const void* heapProps, uint32_t heapFlags, const void* desc, uint32_t initialState, const void* clear, const GUID&, void** out);
  /* 0xe0 */ HRESULT CreateHeap(const void*, const GUID&, void**); /* 0xe8 */ HRESULT CreatePlacedResource(void* heap, uint64_t off, const void* desc, uint32_t state, const void* clear, const GUID&, void**);
  /* 0xf8 */ HRESULT CreateSharedHandle(void* obj, const void* sa, uint32_t access, const wchar_t* name, void** handle); /* 0x100 */ HRESULT OpenSharedHandle(void* handle, const GUID&, void** out);
  /* 0x120 */ HRESULT CreateFence(uint64_t init, uint32_t flags, const GUID&, void** out); /* 0x128 */ HRESULT GetDeviceRemovedReason();
  /* 0x130 */ void GetCopyableFootprints(const void* desc, uint32_t first, uint32_t num, uint64_t base, void* layouts, uint32_t* rows, uint64_t* rowSize, uint64_t* total);
  /* 0x138 */ HRESULT CreateQueryHeap(const void* desc, const GUID&, void** out); };
}
