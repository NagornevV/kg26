#include "GBuffer.h"
#include <stdexcept>

static void ThrowIfFailedLocal(HRESULT hr)
{
    if (FAILED(hr)) throw std::runtime_error("HRESULT failed in GBuffer");
}

void GBuffer::Initialize(ID3D12Device* device, UINT width, UINT height,
    ID3D12DescriptorHeap* srvHeap, UINT srvStartIndex,
    UINT cbvSrvUavDescriptorSize)
{
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = BufferCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailedLocal(device->CreateDescriptorHeap(
        &rtvHeapDesc, IID_PPV_ARGS(&mRtvHeap)));

    mRtvDescriptorSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    Resize(device, width, height, srvHeap, srvStartIndex, cbvSrvUavDescriptorSize);
}

void GBuffer::Resize(ID3D12Device* device, UINT width, UINT height,
    ID3D12DescriptorHeap* srvHeap, UINT srvStartIndex,
    UINT cbvSrvUavDescriptorSize)
{
    if (width == 0 || height == 0) return;
    mWidth = width;
    mHeight = height;

    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);

    for (int i = 0; i < BufferCount; ++i)
    {
        D3D12_CLEAR_VALUE clearValue = {};
        clearValue.Format = mFormats[i];
        clearValue.Color[0] = 0.0f;
        clearValue.Color[1] = 0.0f;
        clearValue.Color[2] = 0.0f;
        clearValue.Color[3] = 1.0f;

        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = mWidth;
        texDesc.Height = mHeight;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = mFormats[i];
        texDesc.SampleDesc.Count = 1;
        texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        ThrowIfFailedLocal(device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clearValue, IID_PPV_ARGS(&mBuffers[i])));
    }

    CreateDescriptors(device, srvHeap, srvStartIndex, cbvSrvUavDescriptorSize);
}

void GBuffer::CreateDescriptors(ID3D12Device* device,
    ID3D12DescriptorHeap* srvHeap, UINT srvStartIndex,
    UINT cbvSrvUavDescriptorSize)
{
    for (int i = 0; i < BufferCount; ++i)
    {
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(
            mRtvHeap->GetCPUDescriptorHandleForHeapStart(), i, mRtvDescriptorSize);
        device->CreateRenderTargetView(mBuffers[i].Get(), nullptr, rtvHandle);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = mFormats[i];
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;

        CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(
            srvHeap->GetCPUDescriptorHandleForHeapStart(),
            srvStartIndex + i, cbvSrvUavDescriptorSize);
        device->CreateShaderResourceView(mBuffers[i].Get(), &srvDesc, srvHandle);
    }
}

void GBuffer::Clear(ID3D12GraphicsCommandList* cmdList)
{
    const float clearColor[] = { 0.f, 0.f, 0.f, 1.f };
    for (int i = 0; i < BufferCount; ++i)
        cmdList->ClearRenderTargetView(Rtv(i), clearColor, 0, nullptr);
}

void GBuffer::TransitionToRenderTargets(ID3D12GraphicsCommandList* cmdList)
{
    D3D12_RESOURCE_BARRIER barriers[BufferCount] = {};
    for (int i = 0; i < BufferCount; ++i)
        barriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(
            mBuffers[i].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmdList->ResourceBarrier(BufferCount, barriers);
}

void GBuffer::TransitionToShaderResources(ID3D12GraphicsCommandList* cmdList)
{
    D3D12_RESOURCE_BARRIER barriers[BufferCount] = {};
    for (int i = 0; i < BufferCount; ++i)
        barriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(
            mBuffers[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(BufferCount, barriers);
}

D3D12_CPU_DESCRIPTOR_HANDLE GBuffer::Rtv(int i) const
{
    return CD3DX12_CPU_DESCRIPTOR_HANDLE(
        mRtvHeap->GetCPUDescriptorHandleForHeapStart(), i, mRtvDescriptorSize);
}

D3D12_GPU_DESCRIPTOR_HANDLE GBuffer::SrvGpuStart(ID3D12DescriptorHeap* srvHeap,
    UINT srvStartIndex, UINT cbvSrvUavDescriptorSize) const
{
    return CD3DX12_GPU_DESCRIPTOR_HANDLE(
        srvHeap->GetGPUDescriptorHandleForHeapStart(),
        srvStartIndex, cbvSrvUavDescriptorSize);
}
