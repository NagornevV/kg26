#include "RenderingSystem.h"
#include <stdexcept>

#pragma comment(lib, "d3dcompiler.lib")

static void ThrowIfFailedRS(HRESULT hr)
{
    if (FAILED(hr)) throw std::runtime_error("HRESULT failed in RenderingSystem");
}

static ComPtr<ID3DBlob> CompileShader(const wchar_t* file, const char* entry, const char* target)
{
    UINT flags = 0;
#ifdef _DEBUG
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> byteCode;
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompileFromFile(file, nullptr, nullptr, entry, target,
        flags, 0, &byteCode, &errors);
    if (FAILED(hr) && errors)
        OutputDebugStringA((char*)errors->GetBufferPointer());
    ThrowIfFailedRS(hr);
    return byteCode;
}

void RenderingSystem::Initialize(ID3D12Device* device,
    DXGI_FORMAT backBufferFormat, DXGI_FORMAT depthStencilFormat,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& instancedInputLayout,
    GBuffer* gbuffer)
{
    BuildGeometryRootSignature(device);
    BuildLightingRootSignature(device);
    BuildShadowRootSignature(device);
    BuildParticleRootSignatures(device);
    BuildShaders();
    BuildPSOs(device, backBufferFormat, depthStencilFormat, inputLayout,
        instancedInputLayout, gbuffer);

    UINT cbSize = (sizeof(CBFrameLights) + 255) & ~255;
    CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
    auto cbDesc = CD3DX12_RESOURCE_DESC::Buffer(cbSize);
    ThrowIfFailedRS(device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&mLightCB)));
    ThrowIfFailedRS(mLightCB->Map(0, nullptr, reinterpret_cast<void**>(&mLightMappedData)));
}

void RenderingSystem::UpdateLights(ID3D12Device* device, ID3D12DescriptorHeap* srvHeap,
    UINT cbvIndex, UINT descriptorSize, const CBFrameLights& lights)
{
    memcpy(mLightMappedData, &lights, sizeof(CBFrameLights));

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = mLightCB->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = (sizeof(CBFrameLights) + 255) & ~255;
    CD3DX12_CPU_DESCRIPTOR_HANDLE h(
        srvHeap->GetCPUDescriptorHandleForHeapStart(), cbvIndex, descriptorSize);
    device->CreateConstantBufferView(&cbvDesc, h);
}

void RenderingSystem::BeginGeometryPass(ID3D12GraphicsCommandList* cmdList, GBuffer* gbuffer,
    D3D12_CPU_DESCRIPTOR_HANDLE dsv, bool wireframe)
{
    cmdList->SetPipelineState(wireframe ? mGeometryWirePSO.Get() : mGeometryPSO.Get());
    cmdList->SetGraphicsRootSignature(mGeometryRootSignature.Get());

    gbuffer->TransitionToRenderTargets(cmdList);
    gbuffer->Clear(cmdList);
    cmdList->ClearDepthStencilView(dsv,
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.f, 0, 0, nullptr);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[GBuffer::BufferCount] =
    {
        gbuffer->Rtv(0), gbuffer->Rtv(1), gbuffer->Rtv(2)
    };
    cmdList->OMSetRenderTargets(GBuffer::BufferCount, rtvs, false, &dsv);
}

void RenderingSystem::BeginTessellationPass(ID3D12GraphicsCommandList* cmdList, bool wireframe)
{
    cmdList->SetPipelineState(wireframe ? mTessellationWirePSO.Get() : mTessellationPSO.Get());
    cmdList->SetGraphicsRootSignature(mGeometryRootSignature.Get());
}

void RenderingSystem::BeginInstancedGeometryPass(ID3D12GraphicsCommandList* cmdList,
    bool wireframe)
{
    cmdList->SetPipelineState(wireframe ? mInstancedGeometryWirePSO.Get()
        : mInstancedGeometryPSO.Get());
    cmdList->SetGraphicsRootSignature(mGeometryRootSignature.Get());
}

void RenderingSystem::BeginParticleCompute(ID3D12GraphicsCommandList* cmdList,
    ID3D12DescriptorHeap* srvHeap, UINT inputUavIndex, UINT outputUavIndex,
    UINT descriptorSize, D3D12_GPU_VIRTUAL_ADDRESS particleComputeCb)
{
    cmdList->SetPipelineState(mParticleComputePSO.Get());
    cmdList->SetComputeRootSignature(mParticleComputeRootSignature.Get());
    cmdList->SetComputeRootConstantBufferView(0, particleComputeCb);

    CD3DX12_GPU_DESCRIPTOR_HANDLE input(
        srvHeap->GetGPUDescriptorHandleForHeapStart(), inputUavIndex, descriptorSize);
    CD3DX12_GPU_DESCRIPTOR_HANDLE output(
        srvHeap->GetGPUDescriptorHandleForHeapStart(), outputUavIndex, descriptorSize);
    cmdList->SetComputeRootDescriptorTable(1, input);
    cmdList->SetComputeRootDescriptorTable(2, output);
}

void RenderingSystem::BeginParticlePass(ID3D12GraphicsCommandList* cmdList, GBuffer* gbuffer,
    D3D12_CPU_DESCRIPTOR_HANDLE dsv, ID3D12DescriptorHeap* srvHeap,
    UINT particleSrvIndex, UINT descriptorSize,
    D3D12_GPU_VIRTUAL_ADDRESS particleRenderCb)
{
    cmdList->SetPipelineState(mParticlePSO.Get());
    cmdList->SetGraphicsRootSignature(mParticleRootSignature.Get());
    cmdList->SetGraphicsRootConstantBufferView(0, particleRenderCb);

    CD3DX12_GPU_DESCRIPTOR_HANDLE particles(
        srvHeap->GetGPUDescriptorHandleForHeapStart(), particleSrvIndex, descriptorSize);
    cmdList->SetGraphicsRootDescriptorTable(1, particles);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[GBuffer::BufferCount] =
    {
        gbuffer->Rtv(0), gbuffer->Rtv(1), gbuffer->Rtv(2)
    };
    cmdList->OMSetRenderTargets(GBuffer::BufferCount, rtvs, false, &dsv);
}

void RenderingSystem::BeginShadowPass(ID3D12GraphicsCommandList* cmdList,
    D3D12_CPU_DESCRIPTOR_HANDLE dsv, const D3D12_VIEWPORT& viewport,
    const D3D12_RECT& scissor)
{
    cmdList->SetPipelineState(mShadowPSO.Get());
    cmdList->SetGraphicsRootSignature(mShadowRootSignature.Get());
    cmdList->RSSetViewports(1, &viewport);
    cmdList->RSSetScissorRects(1, &scissor);
    cmdList->OMSetRenderTargets(0, nullptr, false, &dsv);
    cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
}

void RenderingSystem::BeginLightingPass(ID3D12GraphicsCommandList* cmdList,
    ID3D12DescriptorHeap* srvHeap, UINT gbufferSrvIndex,
    UINT lightCbvIndex, UINT descriptorSize)
{
    cmdList->SetPipelineState(mLightingPSO.Get());
    cmdList->SetGraphicsRootSignature(mLightingRootSignature.Get());

    CD3DX12_GPU_DESCRIPTOR_HANDLE gbuf(
        srvHeap->GetGPUDescriptorHandleForHeapStart(), gbufferSrvIndex, descriptorSize);
    CD3DX12_GPU_DESCRIPTOR_HANDLE lightCbv(
        srvHeap->GetGPUDescriptorHandleForHeapStart(), lightCbvIndex, descriptorSize);

    cmdList->SetGraphicsRootDescriptorTable(0, gbuf);
    cmdList->SetGraphicsRootDescriptorTable(1, lightCbv);
}

void RenderingSystem::BuildGeometryRootSignature(ID3D12Device* device)
{
    CD3DX12_DESCRIPTOR_RANGE cbvRange;
    cbvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
    CD3DX12_DESCRIPTOR_RANGE albedoRange;
    albedoRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
    CD3DX12_DESCRIPTOR_RANGE normalRange;
    normalRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
    CD3DX12_DESCRIPTOR_RANGE displacementRange;
    displacementRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);

    CD3DX12_ROOT_PARAMETER params[4];
    params[0].InitAsDescriptorTable(1, &cbvRange, D3D12_SHADER_VISIBILITY_ALL);
    params[1].InitAsDescriptorTable(1, &albedoRange, D3D12_SHADER_VISIBILITY_PIXEL);
    params[2].InitAsDescriptorTable(1, &normalRange, D3D12_SHADER_VISIBILITY_PIXEL);
    params[3].InitAsDescriptorTable(1, &displacementRange, D3D12_SHADER_VISIBILITY_ALL);

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    CD3DX12_ROOT_SIGNATURE_DESC desc(4, params, 1, &sampler,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    ComPtr<ID3DBlob> serialized, errors;
    ThrowIfFailedRS(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized, &errors));
    ThrowIfFailedRS(device->CreateRootSignature(0, serialized->GetBufferPointer(),
        serialized->GetBufferSize(), IID_PPV_ARGS(&mGeometryRootSignature)));
}

void RenderingSystem::BuildLightingRootSignature(ID3D12Device* device)
{
    CD3DX12_DESCRIPTOR_RANGE gbufferRange;
    gbufferRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, GBuffer::BufferCount, 0);
    CD3DX12_DESCRIPTOR_RANGE lightCbvRange;
    lightCbvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 1);
    CD3DX12_DESCRIPTOR_RANGE shadowRange;
    shadowRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3);

    CD3DX12_ROOT_PARAMETER params[3];
    params[0].InitAsDescriptorTable(1, &gbufferRange, D3D12_SHADER_VISIBILITY_PIXEL);
    params[1].InitAsDescriptorTable(1, &lightCbvRange, D3D12_SHADER_VISIBILITY_PIXEL);
    params[2].InitAsDescriptorTable(1, &shadowRange, D3D12_SHADER_VISIBILITY_PIXEL);

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC shadowSampler = {};
    shadowSampler.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    shadowSampler.AddressU = shadowSampler.AddressV = shadowSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    shadowSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    shadowSampler.MaxLOD = D3D12_FLOAT32_MAX;
    shadowSampler.ShaderRegister = 1;
    shadowSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC samplers[] = { sampler, shadowSampler };

    CD3DX12_ROOT_SIGNATURE_DESC desc(3, params, 2, samplers,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    ComPtr<ID3DBlob> serialized, errors;
    ThrowIfFailedRS(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized, &errors));
    ThrowIfFailedRS(device->CreateRootSignature(0, serialized->GetBufferPointer(),
        serialized->GetBufferSize(), IID_PPV_ARGS(&mLightingRootSignature)));
}

void RenderingSystem::BuildShadowRootSignature(ID3D12Device* device)
{
    CD3DX12_ROOT_PARAMETER parameter;
    parameter.InitAsConstantBufferView(2, 0, D3D12_SHADER_VISIBILITY_VERTEX);
    CD3DX12_ROOT_SIGNATURE_DESC desc(1, &parameter, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    ComPtr<ID3DBlob> serialized, errors;
    ThrowIfFailedRS(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized, &errors));
    ThrowIfFailedRS(device->CreateRootSignature(0, serialized->GetBufferPointer(),
        serialized->GetBufferSize(), IID_PPV_ARGS(&mShadowRootSignature)));
}

void RenderingSystem::BuildParticleRootSignatures(ID3D12Device* device)
{
    CD3DX12_DESCRIPTOR_RANGE inputUavRange;
    inputUavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
    CD3DX12_DESCRIPTOR_RANGE outputUavRange;
    outputUavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1);
    CD3DX12_ROOT_PARAMETER computeParams[3];
    computeParams[0].InitAsConstantBufferView(4);
    computeParams[1].InitAsDescriptorTable(1, &inputUavRange);
    computeParams[2].InitAsDescriptorTable(1, &outputUavRange);
    CD3DX12_ROOT_SIGNATURE_DESC computeDesc(3, computeParams, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_NONE);
    ComPtr<ID3DBlob> serialized, errors;
    ThrowIfFailedRS(D3D12SerializeRootSignature(&computeDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized, &errors));
    ThrowIfFailedRS(device->CreateRootSignature(0, serialized->GetBufferPointer(),
        serialized->GetBufferSize(), IID_PPV_ARGS(&mParticleComputeRootSignature)));

    CD3DX12_DESCRIPTOR_RANGE particleSrvRange;
    particleSrvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4);
    CD3DX12_ROOT_PARAMETER particleParams[2];
    particleParams[0].InitAsConstantBufferView(3);
    particleParams[1].InitAsDescriptorTable(1, &particleSrvRange,
        D3D12_SHADER_VISIBILITY_VERTEX);
    CD3DX12_ROOT_SIGNATURE_DESC particleDesc(2, particleParams, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    serialized.Reset();
    errors.Reset();
    ThrowIfFailedRS(D3D12SerializeRootSignature(&particleDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized, &errors));
    ThrowIfFailedRS(device->CreateRootSignature(0, serialized->GetBufferPointer(),
        serialized->GetBufferSize(), IID_PPV_ARGS(&mParticleRootSignature)));
}

void RenderingSystem::BuildShaders()
{
    mGeometryVS = CompileShader(L"shader.hlsl", "GeometryVS", "vs_5_0");
    mInstancedGeometryVS = CompileShader(L"shader.hlsl", "InstancedGeometryVS", "vs_5_0");
    mGeometryPS = CompileShader(L"shader.hlsl", "GeometryPS", "ps_5_0");
    mTessellationVS = CompileShader(L"shader.hlsl", "TessellationVS", "vs_5_0");
    mTessellationHS = CompileShader(L"shader.hlsl", "TessellationHS", "hs_5_0");
    mTessellationDS = CompileShader(L"shader.hlsl", "TessellationDS", "ds_5_0");
    mLightingVS = CompileShader(L"shader.hlsl", "LightingVS", "vs_5_0");
    mLightingPS = CompileShader(L"shader.hlsl", "LightingPS", "ps_5_0");
    mShadowVS = CompileShader(L"shader.hlsl", "ShadowVS", "vs_5_0");
    mParticleCS = CompileShader(L"shader.hlsl", "ParticleCS", "cs_5_0");
    mParticleVS = CompileShader(L"shader.hlsl", "ParticleVS", "vs_5_0");
    mParticleGS = CompileShader(L"shader.hlsl", "ParticleGS", "gs_5_0");
    mParticlePS = CompileShader(L"shader.hlsl", "ParticlePS", "ps_5_0");
}

void RenderingSystem::BuildPSOs(ID3D12Device* device, DXGI_FORMAT backBufferFormat,
    DXGI_FORMAT depthStencilFormat,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& inputLayout,
    const std::vector<D3D12_INPUT_ELEMENT_DESC>& instancedInputLayout,
    GBuffer* gbuffer)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC geo = {};
    geo.InputLayout = { inputLayout.data(), (UINT)inputLayout.size() };
    geo.pRootSignature = mGeometryRootSignature.Get();
    geo.VS = { mGeometryVS->GetBufferPointer(), mGeometryVS->GetBufferSize() };
    geo.PS = { mGeometryPS->GetBufferPointer(), mGeometryPS->GetBufferSize() };
    geo.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    geo.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    geo.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    geo.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    geo.SampleMask = UINT_MAX;
    geo.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    geo.NumRenderTargets = GBuffer::BufferCount;
    geo.RTVFormats[0] = gbuffer->Format(0);
    geo.RTVFormats[1] = gbuffer->Format(1);
    geo.RTVFormats[2] = gbuffer->Format(2);
    geo.DSVFormat = depthStencilFormat;
    geo.SampleDesc = { 1, 0 };
    ThrowIfFailedRS(device->CreateGraphicsPipelineState(&geo, IID_PPV_ARGS(&mGeometryPSO)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC wire = geo;
    wire.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    ThrowIfFailedRS(device->CreateGraphicsPipelineState(&wire, IID_PPV_ARGS(&mGeometryWirePSO)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC instanced = geo;
    instanced.InputLayout = { instancedInputLayout.data(), (UINT)instancedInputLayout.size() };
    instanced.VS = { mInstancedGeometryVS->GetBufferPointer(), mInstancedGeometryVS->GetBufferSize() };
    ThrowIfFailedRS(device->CreateGraphicsPipelineState(
        &instanced, IID_PPV_ARGS(&mInstancedGeometryPSO)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC instancedWire = instanced;
    instancedWire.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    ThrowIfFailedRS(device->CreateGraphicsPipelineState(
        &instancedWire, IID_PPV_ARGS(&mInstancedGeometryWirePSO)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC tessellation = geo;
    tessellation.VS = { mTessellationVS->GetBufferPointer(), mTessellationVS->GetBufferSize() };
    tessellation.HS = { mTessellationHS->GetBufferPointer(), mTessellationHS->GetBufferSize() };
    tessellation.DS = { mTessellationDS->GetBufferPointer(), mTessellationDS->GetBufferSize() };
    tessellation.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    ThrowIfFailedRS(device->CreateGraphicsPipelineState(
        &tessellation, IID_PPV_ARGS(&mTessellationPSO)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC tessellationWire = tessellation;
    tessellationWire.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    ThrowIfFailedRS(device->CreateGraphicsPipelineState(
        &tessellationWire, IID_PPV_ARGS(&mTessellationWirePSO)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC light = {};
    light.InputLayout = { nullptr, 0 };
    light.pRootSignature = mLightingRootSignature.Get();
    light.VS = { mLightingVS->GetBufferPointer(), mLightingVS->GetBufferSize() };
    light.PS = { mLightingPS->GetBufferPointer(), mLightingPS->GetBufferSize() };
    light.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    light.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    light.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    light.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    light.DepthStencilState.DepthEnable = false;
    light.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    light.SampleMask = UINT_MAX;
    light.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    light.NumRenderTargets = 1;
    light.RTVFormats[0] = backBufferFormat;
    light.SampleDesc = { 1, 0 };
    ThrowIfFailedRS(device->CreateGraphicsPipelineState(&light, IID_PPV_ARGS(&mLightingPSO)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC shadow = {};
    shadow.InputLayout = { inputLayout.data(), (UINT)inputLayout.size() };
    shadow.pRootSignature = mShadowRootSignature.Get();
    shadow.VS = { mShadowVS->GetBufferPointer(), mShadowVS->GetBufferSize() };
    shadow.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    shadow.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    shadow.RasterizerState.DepthBias = 1000;
    shadow.RasterizerState.SlopeScaledDepthBias = 1.5f;
    shadow.RasterizerState.DepthBiasClamp = 0.01f;
    shadow.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    shadow.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    shadow.SampleMask = UINT_MAX;
    shadow.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    shadow.NumRenderTargets = 0;
    shadow.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    shadow.SampleDesc = { 1, 0 };
    ThrowIfFailedRS(device->CreateGraphicsPipelineState(&shadow, IID_PPV_ARGS(&mShadowPSO)));

    D3D12_COMPUTE_PIPELINE_STATE_DESC particleCompute = {};
    particleCompute.pRootSignature = mParticleComputeRootSignature.Get();
    particleCompute.CS = { mParticleCS->GetBufferPointer(), mParticleCS->GetBufferSize() };
    ThrowIfFailedRS(device->CreateComputePipelineState(&particleCompute,
        IID_PPV_ARGS(&mParticleComputePSO)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC particle = {};
    particle.InputLayout = { nullptr, 0 };
    particle.pRootSignature = mParticleRootSignature.Get();
    particle.VS = { mParticleVS->GetBufferPointer(), mParticleVS->GetBufferSize() };
    particle.GS = { mParticleGS->GetBufferPointer(), mParticleGS->GetBufferSize() };
    particle.PS = { mParticlePS->GetBufferPointer(), mParticlePS->GetBufferSize() };
    particle.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    particle.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    particle.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    particle.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    particle.SampleMask = UINT_MAX;
    particle.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    particle.NumRenderTargets = GBuffer::BufferCount;
    particle.RTVFormats[0] = gbuffer->Format(0);
    particle.RTVFormats[1] = gbuffer->Format(1);
    particle.RTVFormats[2] = gbuffer->Format(2);
    particle.DSVFormat = depthStencilFormat;
    particle.SampleDesc = { 1, 0 };
    ThrowIfFailedRS(device->CreateGraphicsPipelineState(&particle,
        IID_PPV_ARGS(&mParticlePSO)));
}
