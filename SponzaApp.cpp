#include "SponzaApp.h"
#include <stdexcept>

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#pragma comment(lib, "d3dcompiler.lib")

static void ThrowIfFailed(HRESULT hr)
{
    if (FAILED(hr))
        throw std::runtime_error("HRESULT failed");
}

// =============================================================
SponzaApp::SponzaApp(HINSTANCE hInstance)
    : D3DApp(hInstance)
{
    mMainWndCaption = L"Sponza — DX12 | ЛКМ: вращение | Колесико: зум";
}

SponzaApp::~SponzaApp()
{
    if (md3dDevice != nullptr)
        FlushCommandQueue();

    if (mCbMappedData)
    {
        mConstantBuffer->Unmap(0, nullptr);
        mCbMappedData = nullptr;
    }
}

// =============================================================
bool SponzaApp::Initialize()
{
    if (!D3DApp::Initialize())
        return false;

    ThrowIfFailed(mDirectCmdListAlloc->Reset());
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

    BuildDescriptorHeap();
    BuildConstantBuffer();
    BuildRootSignature();
    BuildShadersAndInputLayout();
    CreateWhiteTexture();
    LoadModel("sponza/sponza.obj");
    BuildPSO();

    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(1, cmdsLists);
    FlushCommandQueue();

    mVertexUpload.Reset();
    mIndexUpload.Reset();
    mTextureUploads.clear();

    return true;
}

// =============================================================
void SponzaApp::OnResize()
{
    D3DApp::OnResize();
    XMMATRIX proj = XMMatrixPerspectiveFovLH(
        XM_PIDIV4, AspectRatio(), 1.f, 10000.f);
    XMStoreFloat4x4(&mProj, proj);
}

// =============================================================
//  Управление мышью
// =============================================================
void SponzaApp::OnMouseDown(WPARAM btnState, int x, int y)
{
    mMouseDown    = true;
    mLastMouse.x  = x;
    mLastMouse.y  = y;
    SetCapture(mhMainWnd);
}

void SponzaApp::OnMouseUp(WPARAM btnState, int x, int y)
{
    mMouseDown = false;
    ReleaseCapture();
}

void SponzaApp::OnMouseMove(WPARAM btnState, int x, int y)
{
    if (mMouseDown)
    {
        int dx = x - (int)mLastMouse.x;
        int dy = y - (int)mLastMouse.y;

        mYaw   += dx * mMouseSens;
        mPitch += dy * mMouseSens;

        // Ограничиваем pitch чтобы не перевернуться
        const float limit = XM_PIDIV2 - 0.01f;
        if (mPitch >  limit) mPitch =  limit;
        if (mPitch < -limit) mPitch = -limit;
    }

    mLastMouse.x = x;
    mLastMouse.y = y;
}

void SponzaApp::OnMouseWheel(short delta)
{
    // delta > 0 — крутим от себя = приближаем
    mRadius -= delta * mZoomSpeed * 0.01f;

    // Ограничиваем расстояние
    if (mRadius < 100.f)   mRadius = 100.f;
    if (mRadius > 3000.f)  mRadius = 3000.f;
}

// =============================================================
void SponzaApp::Update(const GameTimer& gt)
{
    // Позиция камеры из сферических координат (yaw + pitch + radius)
    mEyePos = {
        mRadius * cosf(mPitch) * sinf(mYaw),
        mRadius * sinf(mPitch) + 100.f,     // +100 чтобы не уходить под пол
        mRadius * cosf(mPitch) * cosf(mYaw)
    };

    XMMATRIX world = XMMatrixIdentity();
    XMMATRIX view  = XMMatrixLookAtLH(
        XMLoadFloat3(&mEyePos),
        XMVectorSet(0.f, 100.f, 0.f, 0.f),
        XMVectorSet(0.f,   1.f, 0.f, 0.f));
    XMMATRIX proj = XMLoadFloat4x4(&mProj);

    // Анимация UV
    mUVOffset.x = fmodf(gt.TotalTime() * mUVScrollSpeed,        1.f);
    mUVOffset.y = fmodf(gt.TotalTime() * mUVScrollSpeed * 0.5f, 1.f);

    CBPerObject cb = {};
    XMStoreFloat4x4(&cb.World,    XMMatrixTranspose(world));
    XMStoreFloat4x4(&cb.ViewProj, XMMatrixTranspose(view * proj));
    cb.LightDir    = { 0.3f, -1.f, 0.5f };
    cb.LightColor  = { 1.f,  1.f,  1.f  };
    cb.EyePos      = mEyePos;
    cb.ObjectColor = { 1.f, 1.f, 1.f };
    cb.UVScale     = mUVScale;
    cb.UVOffset    = mUVOffset;

    memcpy(mCbMappedData, &cb, sizeof(CBPerObject));
}

// =============================================================
void SponzaApp::Draw(const GameTimer& gt)
{
    ThrowIfFailed(mDirectCmdListAlloc->Reset());
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), mPSO.Get()));

    mCommandList->RSSetViewports(1, &mScreenViewport);
    mCommandList->RSSetScissorRects(1, &mScissorRect);

    auto toRT = CD3DX12_RESOURCE_BARRIER::Transition(
        CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    mCommandList->ResourceBarrier(1, &toRT);

    float bg[] = { 0.05f, 0.07f, 0.1f, 1.f };
    mCommandList->ClearRenderTargetView(CurrentBackBufferView(), bg, 0, nullptr);
    mCommandList->ClearDepthStencilView(
        DepthStencilView(),
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
        1.f, 0, 0, nullptr);

    auto cbv = CurrentBackBufferView();
    auto dsv = DepthStencilView();
    mCommandList->OMSetRenderTargets(1, &cbv, true, &dsv);

    ID3D12DescriptorHeap* heaps[] = { mSrvHeap.Get() };
    mCommandList->SetDescriptorHeaps(1, heaps);
    mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

    mCommandList->SetGraphicsRootDescriptorTable(
        0, mSrvHeap->GetGPUDescriptorHandleForHeapStart());

    mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    mCommandList->IASetVertexBuffers(0, 1, &mVbView);
    mCommandList->IASetIndexBuffer(&mIbView);

    for (auto& sm : mSubMeshes)
    {
        reinterpret_cast<CBPerObject*>(mCbMappedData)->ObjectColor =
            sm.DiffuseColor;

        CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(
            mSrvHeap->GetGPUDescriptorHandleForHeapStart(),
            sm.TextureIndex, mCbvSrvUavDescriptorSize);
        mCommandList->SetGraphicsRootDescriptorTable(1, srvHandle);

        mCommandList->DrawIndexedInstanced(
            sm.IndexCount, 1, sm.IndexStart, 0, 0);
    }

    auto toPresent = CD3DX12_RESOURCE_BARRIER::Transition(
        CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT);
    mCommandList->ResourceBarrier(1, &toPresent);

    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(1, cmdsLists);

    ThrowIfFailed(mSwapChain->Present(1, 0));
    mCurrBackBuffer = (mCurrBackBuffer + 1) % kSwapChainBufferCount;

    FlushCommandQueue();
}

// =============================================================
void SponzaApp::BuildDescriptorHeap()
{
    UINT totalSlots = 1 + 1 + kMaxTextures;

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = totalSlots;
    heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
        &heapDesc, IID_PPV_ARGS(&mSrvHeap)));
}

void SponzaApp::BuildConstantBuffer()
{
    UINT cbSize = (sizeof(CBPerObject) + 255) & ~255;

    CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
    auto cbDesc = CD3DX12_RESOURCE_DESC::Buffer(cbSize);
    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&mConstantBuffer)));

    ThrowIfFailed(mConstantBuffer->Map(0, nullptr,
        reinterpret_cast<void**>(&mCbMappedData)));

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = mConstantBuffer->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes    = cbSize;

    md3dDevice->CreateConstantBufferView(
        &cbvDesc,
        mSrvHeap->GetCPUDescriptorHandleForHeapStart());
}

void SponzaApp::BuildRootSignature()
{
    CD3DX12_DESCRIPTOR_RANGE cbvRange;
    cbvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);

    CD3DX12_DESCRIPTOR_RANGE srvRange;
    srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

    CD3DX12_ROOT_PARAMETER slotRootParameter[2];
    slotRootParameter[0].InitAsDescriptorTable(
        1, &cbvRange, D3D12_SHADER_VISIBILITY_ALL);
    slotRootParameter[1].InitAsDescriptorTable(
        1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxAnisotropy    = 1;
    sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD           = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister   = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
        2, slotRootParameter, 1, &sampler,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> serializedRootSig, errorBlob;
    ThrowIfFailed(D3D12SerializeRootSignature(
        &rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        &serializedRootSig, &errorBlob));

    ThrowIfFailed(md3dDevice->CreateRootSignature(
        0,
        serializedRootSig->GetBufferPointer(),
        serializedRootSig->GetBufferSize(),
        IID_PPV_ARGS(&mRootSignature)));
}

void SponzaApp::BuildShadersAndInputLayout()
{
    UINT compileFlags = 0;
#ifdef _DEBUG
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> errors;

    HRESULT hr = D3DCompileFromFile(
        L"shader.hlsl", nullptr, nullptr,
        "VS", "vs_5_0", compileFlags, 0, &mVsByteCode, &errors);
    if (FAILED(hr) && errors)
        OutputDebugStringA((char*)errors->GetBufferPointer());
    ThrowIfFailed(hr);

    hr = D3DCompileFromFile(
        L"shader.hlsl", nullptr, nullptr,
        "PS", "ps_5_0", compileFlags, 0, &mPsByteCode, &errors);
    if (FAILED(hr) && errors)
        OutputDebugStringA((char*)errors->GetBufferPointer());
    ThrowIfFailed(hr);

    mInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
}

void SponzaApp::BuildPSO()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout           = { mInputLayout.data(), (UINT)mInputLayout.size() };
    psoDesc.pRootSignature        = mRootSignature.Get();
    psoDesc.VS                    = { mVsByteCode->GetBufferPointer(),
                                      mVsByteCode->GetBufferSize() };
    psoDesc.PS                    = { mPsByteCode->GetBufferPointer(),
                                      mPsByteCode->GetBufferSize() };
    psoDesc.RasterizerState       = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState            = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState     = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask            = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets      = 1;
    psoDesc.RTVFormats[0]         = mBackBufferFormat;
    psoDesc.DSVFormat             = mDepthStencilFormat;
    psoDesc.SampleDesc            = { 1, 0 };

    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(
        &psoDesc, IID_PPV_ARGS(&mPSO)));
}

void SponzaApp::UploadBufferData(ComPtr<ID3D12Resource>& dest,
                                  ComPtr<ID3D12Resource>& upload,
                                  const void* data, UINT byteSize,
                                  D3D12_RESOURCE_STATES finalState)
{
    CD3DX12_HEAP_PROPERTIES defHeap(D3D12_HEAP_TYPE_DEFAULT);
    CD3DX12_HEAP_PROPERTIES upHeap(D3D12_HEAP_TYPE_UPLOAD);
    auto bufDesc = CD3DX12_RESOURCE_DESC::Buffer(byteSize);

    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &defHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&dest)));

    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &upHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&upload)));

    void* mapped = nullptr;
    upload->Map(0, nullptr, &mapped);
    memcpy(mapped, data, byteSize);
    upload->Unmap(0, nullptr);

    mCommandList->CopyBufferRegion(dest.Get(), 0, upload.Get(), 0, byteSize);

    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        dest.Get(), D3D12_RESOURCE_STATE_COPY_DEST, finalState);
    mCommandList->ResourceBarrier(1, &barrier);
}

void SponzaApp::CreateWhiteTexture()
{
    UINT32 white = 0xFFFFFFFF;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width            = 1;
    texDesc.Height           = 1;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels        = 1;
    texDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc       = { 1, 0 };

    CD3DX12_HEAP_PROPERTIES defHeap(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> tex;
    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &defHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&tex)));

    UINT64 uploadSize = 0;
    md3dDevice->GetCopyableFootprints(&texDesc, 0, 1, 0,
        nullptr, nullptr, nullptr, &uploadSize);

    CD3DX12_HEAP_PROPERTIES upHeap(D3D12_HEAP_TYPE_UPLOAD);
    auto upDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
    ComPtr<ID3D12Resource> upload;
    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &upHeap, D3D12_HEAP_FLAG_NONE, &upDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&upload)));

    D3D12_SUBRESOURCE_DATA subData = {};
    subData.pData      = &white;
    subData.RowPitch   = 4;
    subData.SlicePitch = 4;
    UpdateSubresources(mCommandList.Get(), tex.Get(), upload.Get(), 0, 0, 1, &subData);

    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        tex.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    mCommandList->ResourceBarrier(1, &barrier);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels     = 1;

    CD3DX12_CPU_DESCRIPTOR_HANDLE hWhite(
        mSrvHeap->GetCPUDescriptorHandleForHeapStart(),
        1, mCbvSrvUavDescriptorSize);
    md3dDevice->CreateShaderResourceView(tex.Get(), &srvDesc, hWhite);

    mTextures.push_back(tex);
    mTextureUploads.push_back(upload);
}

bool SponzaApp::LoadTexture(const std::string& path, int heapIndex)
{
    int w, h, channels;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!pixels)
        return false;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width            = (UINT)w;
    texDesc.Height           = (UINT)h;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels        = 1;
    texDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc       = { 1, 0 };

    CD3DX12_HEAP_PROPERTIES defHeap(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> tex;
    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &defHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&tex)));

    UINT64 uploadSize = 0;
    md3dDevice->GetCopyableFootprints(&texDesc, 0, 1, 0,
        nullptr, nullptr, nullptr, &uploadSize);

    CD3DX12_HEAP_PROPERTIES upHeap(D3D12_HEAP_TYPE_UPLOAD);
    auto upDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
    ComPtr<ID3D12Resource> upload;
    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &upHeap, D3D12_HEAP_FLAG_NONE, &upDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&upload)));

    D3D12_SUBRESOURCE_DATA subData = {};
    subData.pData      = pixels;
    subData.RowPitch   = (LONG_PTR)w * 4;
    subData.SlicePitch = subData.RowPitch * h;
    UpdateSubresources(mCommandList.Get(), tex.Get(), upload.Get(), 0, 0, 1, &subData);

    stbi_image_free(pixels);

    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        tex.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    mCommandList->ResourceBarrier(1, &barrier);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels     = 1;

    CD3DX12_CPU_DESCRIPTOR_HANDLE hTex(
        mSrvHeap->GetCPUDescriptorHandleForHeapStart(),
        heapIndex, mCbvSrvUavDescriptorSize);
    md3dDevice->CreateShaderResourceView(tex.Get(), &srvDesc, hTex);

    mTextures.push_back(tex);
    mTextureUploads.push_back(upload);
    return true;
}

void SponzaApp::LoadModel(const std::string& objPath)
{
    tinyobj::attrib_t                attrib;
    std::vector<tinyobj::shape_t>    shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    std::string baseDir =
        objPath.substr(0, objPath.find_last_of("/\\") + 1);

    bool ok = tinyobj::LoadObj(
        &attrib, &shapes, &materials,
        &warn, &err,
        objPath.c_str(), baseDir.c_str());

    if (!ok) throw std::runtime_error("Failed to load OBJ");

    std::vector<Vertex>   allVerts;
    std::vector<uint32_t> allIndices;

    for (auto& shape : shapes)
    {
        SubMesh sm;
        sm.IndexStart   = (UINT)allIndices.size();
        sm.DiffuseColor = { 1.f, 1.f, 1.f };
        sm.TextureIndex = 1;

        int matId = -1;
        if (!shape.mesh.material_ids.empty())
            matId = shape.mesh.material_ids[0];

        if (matId >= 0 && matId < (int)materials.size())
        {
            auto& mat = materials[matId];

            sm.DiffuseColor = {
                mat.diffuse[0] > 0.01f ? mat.diffuse[0] : 1.f,
                mat.diffuse[1] > 0.01f ? mat.diffuse[1] : 1.f,
                mat.diffuse[2] > 0.01f ? mat.diffuse[2] : 1.f
            };

            std::string texName = mat.diffuse_texname;
            if (!texName.empty())
            {
                for (char& c : texName)
                    if (c == '\\') c = '/';

                std::string fullPath = baseDir + texName;

                auto it = mTextureMap.find(fullPath);
                if (it != mTextureMap.end())
                {
                    sm.TextureIndex = it->second;
                }
                else if (mNextTexSlot < 1 + 1 + kMaxTextures)
                {
                    int slot = mNextTexSlot;
                    if (LoadTexture(fullPath, slot))
                    {
                        mNextTexSlot++;
                        mTextureMap[fullPath] = slot;
                        sm.TextureIndex = slot;
                    }
                    else
                    {
                        sm.TextureIndex = 1;
                    }
                }
            }
        }

        for (auto& idx : shape.mesh.indices)
        {
            Vertex v = {};

            v.Pos = {
                attrib.vertices[3 * idx.vertex_index + 0],
                attrib.vertices[3 * idx.vertex_index + 1],
                attrib.vertices[3 * idx.vertex_index + 2]
            };

            if (idx.normal_index >= 0)
            {
                v.Normal = {
                    attrib.normals[3 * idx.normal_index + 0],
                    attrib.normals[3 * idx.normal_index + 1],
                    attrib.normals[3 * idx.normal_index + 2]
                };
            }

            if (idx.texcoord_index >= 0)
            {
                v.TexC = {
                    attrib.texcoords[2 * idx.texcoord_index + 0],
                    1.f - attrib.texcoords[2 * idx.texcoord_index + 1]
                };
            }

            allIndices.push_back((uint32_t)allVerts.size());
            allVerts.push_back(v);
        }

        sm.IndexCount = (UINT)allIndices.size() - sm.IndexStart;
        mSubMeshes.push_back(sm);
    }

    UINT vbSize = (UINT)(allVerts.size()   * sizeof(Vertex));
    UINT ibSize = (UINT)(allIndices.size() * sizeof(uint32_t));

    UploadBufferData(mVertexBuffer, mVertexUpload,
        allVerts.data(), vbSize,
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

    UploadBufferData(mIndexBuffer, mIndexUpload,
        allIndices.data(), ibSize,
        D3D12_RESOURCE_STATE_INDEX_BUFFER);

    mVbView.BufferLocation = mVertexBuffer->GetGPUVirtualAddress();
    mVbView.SizeInBytes    = vbSize;
    mVbView.StrideInBytes  = sizeof(Vertex);

    mIbView.BufferLocation = mIndexBuffer->GetGPUVirtualAddress();
    mIbView.SizeInBytes    = ibSize;
    mIbView.Format         = DXGI_FORMAT_R32_UINT;
}

void SponzaApp::BuildGeometry()
{
    LoadModel("sponza/sponza.obj");
}
