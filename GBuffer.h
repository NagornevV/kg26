#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include "d3dx12.h"

using Microsoft::WRL::ComPtr;

// GBuffer хранит геометрию сцены для deferred rendering:
// 0 - Albedo, 1 - Normal, 2 - Position.
class GBuffer
{
public:
    static const int BufferCount = 3;

    void Initialize(ID3D12Device* device, UINT width, UINT height,
        ID3D12DescriptorHeap* srvHeap, UINT srvStartIndex,
        UINT cbvSrvUavDescriptorSize);

    void Resize(ID3D12Device* device, UINT width, UINT height,
        ID3D12DescriptorHeap* srvHeap, UINT srvStartIndex,
        UINT cbvSrvUavDescriptorSize);

    void Clear(ID3D12GraphicsCommandList* cmdList);
    void TransitionToRenderTargets(ID3D12GraphicsCommandList* cmdList);
    void TransitionToShaderResources(ID3D12GraphicsCommandList* cmdList);

    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(int i) const;
    D3D12_GPU_DESCRIPTOR_HANDLE SrvGpuStart(ID3D12DescriptorHeap* srvHeap,
        UINT srvStartIndex, UINT cbvSrvUavDescriptorSize) const;

    DXGI_FORMAT Format(int i) const { return mFormats[i]; }

private:
    void CreateDescriptors(ID3D12Device* device, ID3D12DescriptorHeap* srvHeap,
        UINT srvStartIndex, UINT cbvSrvUavDescriptorSize);

private:
    UINT mWidth = 0;
    UINT mHeight = 0;
    DXGI_FORMAT mFormats[BufferCount] =
    {
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R16G16B16A16_FLOAT
    };
    ComPtr<ID3D12Resource> mBuffers[BufferCount];
    ComPtr<ID3D12DescriptorHeap> mRtvHeap;
    UINT mRtvDescriptorSize = 0;
};
