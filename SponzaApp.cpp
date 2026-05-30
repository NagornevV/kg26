#include "SponzaApp.h"
#include <stdexcept>
#include <algorithm>
#include <cmath>

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
    mMainWndCaption = L"Sponza — DX12 Deferred Rendering | F: wireframe";
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
    BuildShadersAndInputLayout();

    // GBuffer создаётся до PSO, потому что форматы MRT нужны при создании pipeline state.
    mGBuffer.Initialize(md3dDevice.Get(), mClientWidth, mClientHeight,
        mSrvHeap.Get(), kGBufferSrvStart, mCbvSrvUavDescriptorSize);

    BuildRenderingSystem();
    CreateWhiteTexture();
    LoadModel("sponza/sponza.obj");
    BuildLights();

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

    if (md3dDevice != nullptr && mSrvHeap != nullptr)
    {
        mGBuffer.Resize(md3dDevice.Get(), mClientWidth, mClientHeight,
            mSrvHeap.Get(), kGBufferSrvStart, mCbvSrvUavDescriptorSize);
    }
}

void SponzaApp::OnKeyboardInput(WPARAM key)
{
    if (key == 'F')
        mWireframe = !mWireframe;
}

void SponzaApp::OnMouseDown(WPARAM btnState, int x, int y)
{
    mMouseDown = true;
    mLastMouse.x = x;
    mLastMouse.y = y;
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
        mYaw += dx * mMouseSens;
        mPitch += dy * mMouseSens;
        const float limit = XM_PIDIV2 - 0.01f;
        mPitch = std::clamp(mPitch, -limit, limit);
    }
    mLastMouse.x = x;
    mLastMouse.y = y;
}

void SponzaApp::OnMouseWheel(short delta)
{
    mRadius -= delta * mZoomSpeed * 0.01f;
    mRadius = std::clamp(mRadius, 100.f, 3000.f);
}

// =============================================================
void SponzaApp::BuildLights()
{
    mLights = {};
    mLights.DirLightDir = { 0.3f, 1.0f, 20.4f };
    mLights.DirLightColor = { 0.25f, 0.25f, 0.28f };

    const XMFLOAT3 colors[] =
    {
        { 1.0f, 0.35f, 0.20f }, { 1.20f, 0.55f, 1.0f },
        { 1.25f, 1.0f, 0.35f }, { 1.0f, 0.85f, 0.25f },
        { 1.0f, 0.25f, 0.75f }, { 1.35f, 1.0f, 1.0f }
    };

    const XMFLOAT3 positions[] =
    {
        { -600.f, 220.f, -250.f }, { -250.f, 160.f,  320.f },
        {  120.f, 240.f, -420.f }, {  430.f, 180.f,  280.f },
        {  690.f, 260.f,  -80.f }, { -720.f, 190.f,  140.f }
    };

    mLights.PointCount = 6.0f;
    for (int i = 0; i < 6; ++i)
    {
        mLights.Points[i].Position = positions[i];
        mLights.Points[i].Radius = 650.f;
        mLights.Points[i].Color = colors[i];
        mLights.Points[i].Intensity = 2.5f;
    }

    mLights.SpotCount = 1.0f;
    mLights.Spots[0].Position = { 0.f, 650.f, -250.f };
    mLights.Spots[0].Direction = { 0.f, -1.f, 0.25f };
    mLights.Spots[0].Radius = 900.f;
    mLights.Spots[0].SpotPower = 28.f;
    mLights.Spots[0].Color = { 1.0f, 10.95f, 0.75f };
    mLights.Spots[0].Intensity = 8.0f;
}

// =============================================================
void SponzaApp::Update(const GameTimer& gt)
{
    mEyePos = {
        mRadius * cosf(mPitch) * sinf(mYaw),
        mRadius * sinf(mPitch) + 100.f,
        mRadius * cosf(mPitch) * cosf(mYaw)
    };

    XMMATRIX world = XMMatrixIdentity();
    XMMATRIX view = XMMatrixLookAtLH(
        XMLoadFloat3(&mEyePos),
        XMVectorSet(0.f, 100.f, 0.f, 0.f),
        XMVectorSet(0.f, 1.f, 0.f, 0.f));
    XMMATRIX proj = XMLoadFloat4x4(&mProj);

    mUVOffset.x = fmodf(gt.TotalTime() * mUVScrollSpeed, 1.f);
    mUVOffset.y = fmodf(gt.TotalTime() * mUVScrollSpeed * 0.5f, 1.f);

    CBPerObject cb = {};
    XMStoreFloat4x4(&cb.World, XMMatrixTranspose(world));
    XMStoreFloat4x4(&cb.ViewProj, XMMatrixTranspose(view * proj));
    cb.ObjectColor = { 1.f, 1.f, 1.f };
    cb.UVScale = mUVScale;
    cb.UVOffset = mUVOffset;
    memcpy(mCbMappedData, &cb, sizeof(CBPerObject));

    mLights.EyePos = mEyePos;
    mRenderingSystem.UpdateLights(md3dDevice.Get(), mSrvHeap.Get(),
        kLightCbvIndex, mCbvSrvUavDescriptorSize, mLights);
}

// =============================================================
void SponzaApp::Draw(const GameTimer& gt)
{
    ThrowIfFailed(mDirectCmdListAlloc->Reset());
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

    mCommandList->RSSetViewports(1, &mScreenViewport);
    mCommandList->RSSetScissorRects(1, &mScissorRect);

    ID3D12DescriptorHeap* heaps[] = { mSrvHeap.Get() };
    mCommandList->SetDescriptorHeaps(1, heaps);

    // ---------- 1. Geometry pass: рисуем Sponza в GBuffer ----------
    mRenderingSystem.BeginGeometryPass(mCommandList.Get(), &mGBuffer,
        DepthStencilView(), mWireframe);

    mCommandList->SetGraphicsRootDescriptorTable(
        0, mSrvHeap->GetGPUDescriptorHandleForHeapStart());
    mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    mCommandList->IASetVertexBuffers(0, 1, &mVbView);
    mCommandList->IASetIndexBuffer(&mIbView);

    for (auto& sm : mSubMeshes)
    {
        reinterpret_cast<CBPerObject*>(mCbMappedData)->ObjectColor = sm.DiffuseColor;

        CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(
            mSrvHeap->GetGPUDescriptorHandleForHeapStart(),
            sm.TextureIndex, mCbvSrvUavDescriptorSize);
        mCommandList->SetGraphicsRootDescriptorTable(1, srvHandle);

        mCommandList->DrawIndexedInstanced(sm.IndexCount, 1, sm.IndexStart, 0, 0);
    }

    // ---------- 2. Lighting pass: читаем GBuffer и выводим свет на экран ----------
    mGBuffer.TransitionToShaderResources(mCommandList.Get());

    auto toRT = CD3DX12_RESOURCE_BARRIER::Transition(
        CurrentBackBuffer(), D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    mCommandList->ResourceBarrier(1, &toRT);

    float bg[] = { 0.02f, 0.02f, 0.025f, 1.f };
    mCommandList->ClearRenderTargetView(CurrentBackBufferView(), bg, 0, nullptr);
    auto cbv = CurrentBackBufferView();
    mCommandList->OMSetRenderTargets(1, &cbv, true, nullptr);

    mRenderingSystem.BeginLightingPass(mCommandList.Get(), mSrvHeap.Get(),
        kGBufferSrvStart, kLightCbvIndex, mCbvSrvUavDescriptorSize);
    mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    mCommandList->IASetVertexBuffers(0, 0, nullptr);
    mCommandList->IASetIndexBuffer(nullptr);
    mCommandList->DrawInstanced(3, 1, 0, 0);

    auto toPresent = CD3DX12_RESOURCE_BARRIER::Transition(
        CurrentBackBuffer(), D3D12_RESOURCE_STATE_RENDER_TARGET,
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
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = kTotalSrvSlots;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
        &heapDesc, IID_PPV_ARGS(&mSrvHeap)));
}

// =============================================================
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
    cbvDesc.SizeInBytes = cbSize;

    md3dDevice->CreateConstantBufferView(
        &cbvDesc,
        mSrvHeap->GetCPUDescriptorHandleForHeapStart());
}

// =============================================================
void SponzaApp::BuildShadersAndInputLayout()
{
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

void SponzaApp::BuildRenderingSystem()
{
    mRenderingSystem.Initialize(md3dDevice.Get(), mBackBufferFormat,
        mDepthStencilFormat, mInputLayout, &mGBuffer);
}

// =============================================================
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

// =============================================================
void SponzaApp::CreateWhiteTexture()
{
    UINT32 white = 0xFFFFFFFF;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = 1;
    texDesc.Height = 1;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc = { 1, 0 };

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
    subData.pData = &white;
    subData.RowPitch = 4;
    subData.SlicePitch = 4;
    UpdateSubresources(mCommandList.Get(), tex.Get(), upload.Get(), 0, 0, 1, &subData);

    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        tex.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    mCommandList->ResourceBarrier(1, &barrier);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;

    CD3DX12_CPU_DESCRIPTOR_HANDLE hWhite(
        mSrvHeap->GetCPUDescriptorHandleForHeapStart(),
        1, mCbvSrvUavDescriptorSize);
    md3dDevice->CreateShaderResourceView(tex.Get(), &srvDesc, hWhite);

    mTextures.push_back(tex);
    mTextureUploads.push_back(upload);
}

// =============================================================
bool SponzaApp::LoadTexture(const std::string& path, int heapIndex)
{
    int w, h, channels;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!pixels)
        return false;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = (UINT)w;
    texDesc.Height = (UINT)h;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc = { 1, 0 };

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
    subData.pData = pixels;
    subData.RowPitch = (LONG_PTR)w * 4;
    subData.SlicePitch = subData.RowPitch * h;
    UpdateSubresources(mCommandList.Get(), tex.Get(), upload.Get(), 0, 0, 1, &subData);

    stbi_image_free(pixels);

    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        tex.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    mCommandList->ResourceBarrier(1, &barrier);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;

    CD3DX12_CPU_DESCRIPTOR_HANDLE hTex(
        mSrvHeap->GetCPUDescriptorHandleForHeapStart(),
        heapIndex, mCbvSrvUavDescriptorSize);
    md3dDevice->CreateShaderResourceView(tex.Get(), &srvDesc, hTex);

    mTextures.push_back(tex);
    mTextureUploads.push_back(upload);
    return true;
}

// =============================================================
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
        sm.IndexStart = (UINT)allIndices.size();
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

    UINT vbSize = (UINT)(allVerts.size() * sizeof(Vertex));
    UINT ibSize = (UINT)(allIndices.size() * sizeof(uint32_t));

    UploadBufferData(mVertexBuffer, mVertexUpload,
        allVerts.data(), vbSize,
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

    UploadBufferData(mIndexBuffer, mIndexUpload,
        allIndices.data(), ibSize,
        D3D12_RESOURCE_STATE_INDEX_BUFFER);

    mVbView.BufferLocation = mVertexBuffer->GetGPUVirtualAddress();
    mVbView.SizeInBytes = vbSize;
    mVbView.StrideInBytes = sizeof(Vertex);

    mIbView.BufferLocation = mIndexBuffer->GetGPUVirtualAddress();
    mIbView.SizeInBytes = ibSize;
    mIbView.Format = DXGI_FORMAT_R32_UINT;
}

void SponzaApp::BuildGeometry()
{
    LoadModel("sponza/sponza.obj");
}
