#pragma once

#include <d3d12.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <vector>
#include <wrl/client.h>
#include "d3dx12.h"
#include "GBuffer.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;

struct PointLight
{
    XMFLOAT3 Position; float Radius;
    XMFLOAT3 Color;    float Intensity;
};

struct SpotLight
{
    XMFLOAT3 Position;  float Radius;
    XMFLOAT3 Direction; float SpotPower;
    XMFLOAT3 Color;     float Intensity;
};

struct CBFrameLights
{
    XMFLOAT3 EyePos;       float PointCount;
    XMFLOAT3 DirLightDir;  float SpotCount;
    XMFLOAT3 DirLightColor;float Pad0;
    PointLight Points[16];
    SpotLight  Spots[4];
    XMFLOAT4X4 CameraView;
    XMFLOAT4X4 ShadowViewProj[3];
    XMFLOAT4   CascadeSplits;
    XMFLOAT4   PostProcess; // x: gamma, y: vignette
};

// RenderingSystem разделяет рендер на 2 прохода:
// 1) Geometry pass пишет в GBuffer
// 2) Lighting pass читает GBuffer и считает Directional/Point/Spot свет.
class RenderingSystem
{
public:
    void Initialize(ID3D12Device* device,
        DXGI_FORMAT backBufferFormat,
        DXGI_FORMAT depthStencilFormat,
        const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout,
        const std::vector<D3D12_INPUT_ELEMENT_DESC>& instancedInputLayout,
        GBuffer* gbuffer);

    void UpdateLights(ID3D12Device* device, ID3D12DescriptorHeap* srvHeap,
        UINT cbvIndex, UINT descriptorSize, const CBFrameLights& lights);

    void BeginGeometryPass(ID3D12GraphicsCommandList* cmdList, GBuffer* gbuffer,
        D3D12_CPU_DESCRIPTOR_HANDLE dsv, bool wireframe);

    void BeginTessellationPass(ID3D12GraphicsCommandList* cmdList, bool wireframe);
    void BeginInstancedGeometryPass(ID3D12GraphicsCommandList* cmdList, bool wireframe);
    void BeginParticleCompute(ID3D12GraphicsCommandList* cmdList,
        ID3D12DescriptorHeap* srvHeap, UINT inputUavIndex, UINT outputUavIndex,
        UINT descriptorSize, D3D12_GPU_VIRTUAL_ADDRESS particleComputeCb);
    void BeginParticlePass(ID3D12GraphicsCommandList* cmdList, GBuffer* gbuffer,
        D3D12_CPU_DESCRIPTOR_HANDLE dsv, ID3D12DescriptorHeap* srvHeap,
        UINT particleSrvIndex, UINT descriptorSize,
        D3D12_GPU_VIRTUAL_ADDRESS particleRenderCb);
    void BeginShadowPass(ID3D12GraphicsCommandList* cmdList,
        D3D12_CPU_DESCRIPTOR_HANDLE dsv, const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissor);

    void BeginLightingPass(ID3D12GraphicsCommandList* cmdList,
        ID3D12DescriptorHeap* srvHeap, UINT gbufferSrvIndex,
        UINT lightCbvIndex, UINT descriptorSize);

    ID3D12RootSignature* GeometryRootSignature() const { return mGeometryRootSignature.Get(); }
    ID3D12RootSignature* LightingRootSignature() const { return mLightingRootSignature.Get(); }
    ID3D12RootSignature* ShadowRootSignature() const { return mShadowRootSignature.Get(); }
    ID3D12Resource* LightConstantBuffer() const { return mLightCB.Get(); }
    BYTE* LightMappedData() const { return mLightMappedData; }

private:
    void BuildGeometryRootSignature(ID3D12Device* device);
    void BuildLightingRootSignature(ID3D12Device* device);
    void BuildShadowRootSignature(ID3D12Device* device);
    void BuildParticleRootSignatures(ID3D12Device* device);
    void BuildShaders();
    void BuildPSOs(ID3D12Device* device, DXGI_FORMAT backBufferFormat,
        DXGI_FORMAT depthStencilFormat,
        const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout,
        const std::vector<D3D12_INPUT_ELEMENT_DESC>& instancedInputLayout,
        GBuffer* gbuffer);

private:
    ComPtr<ID3D12RootSignature> mGeometryRootSignature;
    ComPtr<ID3D12RootSignature> mLightingRootSignature;
    ComPtr<ID3D12RootSignature> mShadowRootSignature;
    ComPtr<ID3D12RootSignature> mParticleComputeRootSignature;
    ComPtr<ID3D12RootSignature> mParticleRootSignature;

    ComPtr<ID3DBlob> mGeometryVS;
    ComPtr<ID3DBlob> mInstancedGeometryVS;
    ComPtr<ID3DBlob> mGeometryPS;
    ComPtr<ID3DBlob> mTessellationVS;
    ComPtr<ID3DBlob> mTessellationHS;
    ComPtr<ID3DBlob> mTessellationDS;
    ComPtr<ID3DBlob> mLightingVS;
    ComPtr<ID3DBlob> mLightingPS;
    ComPtr<ID3DBlob> mShadowVS;
    ComPtr<ID3DBlob> mParticleCS;
    ComPtr<ID3DBlob> mParticleVS;
    ComPtr<ID3DBlob> mParticleGS;
    ComPtr<ID3DBlob> mParticlePS;

    ComPtr<ID3D12PipelineState> mGeometryPSO;
    ComPtr<ID3D12PipelineState> mGeometryWirePSO;
    ComPtr<ID3D12PipelineState> mInstancedGeometryPSO;
    ComPtr<ID3D12PipelineState> mInstancedGeometryWirePSO;
    ComPtr<ID3D12PipelineState> mTessellationPSO;
    ComPtr<ID3D12PipelineState> mTessellationWirePSO;
    ComPtr<ID3D12PipelineState> mLightingPSO;
    ComPtr<ID3D12PipelineState> mShadowPSO;
    ComPtr<ID3D12PipelineState> mParticleComputePSO;
    ComPtr<ID3D12PipelineState> mParticlePSO;

    ComPtr<ID3D12Resource> mLightCB;
    BYTE* mLightMappedData = nullptr;
};
