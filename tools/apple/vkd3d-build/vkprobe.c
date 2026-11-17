/* vkd3d probe: create a D3D12 device, clear a 64x64 BGRA8 render target, copy it to a readback buffer and print one pixel.
 * Build: gcc -O1 -w -I../vkd3d/usr/include/vkd3d vkprobe.c -o vkprobe -L../vkd3d/usr/lib -lvkd3d-utils -lvkd3d -Wl,-rpath,$PWD/../vkd3d/usr/lib
 * Run (CPU Vulkan driver only): VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json ./vkprobe */
#define COBJMACROS
#define INITGUID
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <vkd3d_windows.h>
#include <vkd3d_d3d12.h>
#include <vkd3d_utils.h>

#define CHK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAIL %s hr=%08x\n", #x, (unsigned)hr_); return 1; } } while (0)

int main(void)
{
    ID3D12Device *dev;
    CHK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev));

    D3D12_COMMAND_QUEUE_DESC qd = {0};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue *q;
    CHK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q));
    ID3D12CommandAllocator *al;
    CHK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&al));
    ID3D12GraphicsCommandList *cl;
    CHK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, NULL, &IID_ID3D12GraphicsCommandList, (void **)&cl));

    D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    ID3D12DescriptorHeap *rh;
    CHK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rh));

    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = 64; rd.Height = 64; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *rt;
    CHK(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&rt));

    D3D12_CPU_DESCRIPTOR_HANDLE h = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rh);
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, h);
    float col[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    ID3D12GraphicsCommandList_ClearRenderTargetView(cl, h, col, 0, NULL);

    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = rt;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT64 total = 0;
    ID3D12Device_GetCopyableFootprints(dev, &rd, 0, 1, 0, &fp, NULL, NULL, &total);
    D3D12_HEAP_PROPERTIES hr = {0};
    hr.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *rb;
    CHK(ID3D12Device_CreateCommittedResource(dev, &hr, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&rb));

    D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
    src.pResource = rt; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
    ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
    CHK(ID3D12GraphicsCommandList_Close(cl));
    ID3D12CommandList *lists[1] = {(ID3D12CommandList *)cl};
    ID3D12CommandQueue_ExecuteCommandLists(q, 1, lists);

    ID3D12Fence *f;
    CHK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&f));
    CHK(ID3D12CommandQueue_Signal(q, f, 1));
    for (int i = 0; i < 5000 && ID3D12Fence_GetCompletedValue(f) < 1; i++)
        usleep(1000);

    unsigned char *p = NULL;
    D3D12_RANGE rg = {0, (SIZE_T)total};
    CHK(ID3D12Resource_Map(rb, 0, &rg, (void **)&p));
    printf("OK device+render+readback: pixel BGRA = %u %u %u %u (expect 191 128 64 255), row pitch %u, bytes %llu\n",
            p[0], p[1], p[2], p[3], fp.Footprint.RowPitch, (unsigned long long)total);
    return 0;
}
