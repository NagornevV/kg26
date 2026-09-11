#include "SponzaApp.h"
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <limits>

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

static bool RayTriangleIntersection(const XMFLOAT3& origin,
    const XMFLOAT3& direction, const CollisionTriangle& triangle,
    float& distance)
{
    const XMVECTOR rayOrigin = XMLoadFloat3(&origin);
    const XMVECTOR rayDirection = XMLoadFloat3(&direction);
    const XMVECTOR a = XMLoadFloat3(&triangle.A);
    const XMVECTOR b = XMLoadFloat3(&triangle.B);
    const XMVECTOR c = XMLoadFloat3(&triangle.C);

    const XMVECTOR edge1 = XMVectorSubtract(b, a);
    const XMVECTOR edge2 = XMVectorSubtract(c, a);
    const XMVECTOR p = XMVector3Cross(rayDirection, edge2);
    const float determinant = XMVectorGetX(XMVector3Dot(edge1, p));

    if (std::abs(determinant) < 0.000001f)
        return false;

    const float inverseDeterminant = 1.0f / determinant;
    const XMVECTOR t = XMVectorSubtract(rayOrigin, a);
    const float u = XMVectorGetX(XMVector3Dot(t, p)) * inverseDeterminant;
    if (u < 0.0f || u > 1.0f)
        return false;

    const XMVECTOR q = XMVector3Cross(t, edge1);
    const float v = XMVectorGetX(XMVector3Dot(rayDirection, q)) * inverseDeterminant;
    if (v < 0.0f || u + v > 1.0f)
        return false;

    distance = XMVectorGetX(XMVector3Dot(edge2, q)) * inverseDeterminant;
    return distance > 0.01f;
}

// =============================================================
SponzaApp::SponzaApp(HINSTANCE hInstance)
    : D3DApp(hInstance)
{
    mMainWndCaption = L"Sponza - WASD: move | LMB: shoot | RMB: orbit | F: wireframe";
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

    if (mTessellationCbMappedData)
    {
        mTessellationConstantBuffer->Unmap(0, nullptr);
        mTessellationCbMappedData = nullptr;
    }

    if (mInstanceMappedData)
    {
        mInstanceBuffer->Unmap(0, nullptr);
        mInstanceMappedData = nullptr;
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
    BuildTessellationConstantBuffer();
    BuildShadersAndInputLayout();

    // GBuffer создаётся до PSO, потому что форматы MRT нужны при создании pipeline state.
    mGBuffer.Initialize(md3dDevice.Get(), mClientWidth, mClientHeight,
        mSrvHeap.Get(), kGBufferSrvStart, mCbvSrvUavDescriptorSize);

    BuildRenderingSystem();
    CreateWhiteTexture();
    LoadTessellationTextures();
    LoadModel("sponza/sponza.obj");
    BuildTessellatedSurface();
    BuildCubeGeometry();
    BuildSceneObjects();
    BuildOctree();
    BuildInstanceBuffer();
    BuildLights();

    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(1, cmdsLists);
    FlushCommandQueue();

    mVertexUpload.Reset();
    mIndexUpload.Reset();
    mTessellationVertexUpload.Reset();
    mCubeVertexUpload.Reset();
    mCubeIndexUpload.Reset();
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
    else if (key == 'C')
        mFrustumCullingEnabled = !mFrustumCullingEnabled;
    else if (key == 'O')
        mOctreeCullingEnabled = !mOctreeCullingEnabled;

    UpdateWindowCaption();
    SetWindowText(mhMainWnd, mMainWndCaption.c_str());
}

void SponzaApp::OnMouseDown(WPARAM btnState, int x, int y)
{
    if (btnState & MK_LBUTTON)
        ShootLight();

    if (btnState & MK_RBUTTON)
    {
        mMouseDown = true;
        mLastMouse.x = x;
        mLastMouse.y = y;
        SetCapture(mhMainWnd);
    }
}

void SponzaApp::OnMouseUp(WPARAM btnState, int x, int y)
{
    if (mMouseDown)
    {
        mMouseDown = false;
        ReleaseCapture();
    }
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

    mLights.PointCount = (float)kStaticPointLights;
    for (int i = 0; i < kStaticPointLights; ++i)
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
    UpdateCameraMovement(gt.DeltaTime());

    mEyePos = {
        mCameraTarget.x + mRadius * cosf(mPitch) * sinf(mYaw),
        mCameraTarget.y + mRadius * sinf(mPitch),
        mCameraTarget.z + mRadius * cosf(mPitch) * cosf(mYaw)
    };

    XMMATRIX world = XMMatrixIdentity();
    XMMATRIX view = XMMatrixLookAtLH(
        XMLoadFloat3(&mEyePos),
        XMLoadFloat3(&mCameraTarget),
        XMVectorSet(0.f, 1.f, 0.f, 0.f));
    XMMATRIX proj = XMLoadFloat4x4(&mProj);

    mUVOffset.x = fmodf(gt.TotalTime() * mUVScrollSpeed, 1.f);
    mUVOffset.y = fmodf(gt.TotalTime() * mUVScrollSpeed * 0.5f, 1.f);

    UpdateShotLights(gt.DeltaTime());
    UpdateVisibleInstances(view * proj);

    CBPerObject cb = {};
    XMStoreFloat4x4(&cb.World, XMMatrixTranspose(world));
    XMStoreFloat4x4(&cb.ViewProj, XMMatrixTranspose(view * proj));
    cb.ObjectColor = { 1.f, 1.f, 1.f };
    cb.UVScale = mUVScale;
    cb.UVOffset = mUVOffset;
    cb.TessellationMaxFactor = 32.f;
    cb.UseNormalMap = 0.f;
    cb.DisplacementScale = 0.f;
    cb.RenderTargetSize = { static_cast<float>(mClientWidth), static_cast<float>(mClientHeight) };
    memcpy(mCbMappedData, &cb, sizeof(CBPerObject));

    CBPerObject tessellationCb = cb;
    tessellationCb.UVScale = { 5.f, 5.f };
    tessellationCb.UVOffset = { 0.f, 0.f };
    tessellationCb.UseNormalMap = 1.f;
    tessellationCb.DisplacementScale = 35.f;
    memcpy(mTessellationCbMappedData, &tessellationCb, sizeof(CBPerObject));

    mLights.EyePos = mEyePos;
    mRenderingSystem.UpdateLights(md3dDevice.Get(), mSrvHeap.Get(),
        kLightCbvIndex, mCbvSrvUavDescriptorSize, mLights);
    UpdateWindowCaption();
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
        CD3DX12_GPU_DESCRIPTOR_HANDLE whiteHandle(
            mSrvHeap->GetGPUDescriptorHandleForHeapStart(),
            1, mCbvSrvUavDescriptorSize);
        mCommandList->SetGraphicsRootDescriptorTable(2, whiteHandle);
        mCommandList->SetGraphicsRootDescriptorTable(3, whiteHandle);

        mCommandList->DrawIndexedInstanced(sm.IndexCount, 1, sm.IndexStart, 0, 0);
    }

    // ---------- Instanced cubes: frustum culling / octree ----------
    if (mVisibleObjectCount > 0)
    {
        mRenderingSystem.BeginInstancedGeometryPass(mCommandList.Get(), mWireframe);
        mCommandList->SetGraphicsRootDescriptorTable(
            0, mSrvHeap->GetGPUDescriptorHandleForHeapStart());
        CD3DX12_GPU_DESCRIPTOR_HANDLE whiteHandle(
            mSrvHeap->GetGPUDescriptorHandleForHeapStart(),
            1, mCbvSrvUavDescriptorSize);
        mCommandList->SetGraphicsRootDescriptorTable(1, whiteHandle);
        mCommandList->SetGraphicsRootDescriptorTable(2, whiteHandle);
        mCommandList->SetGraphicsRootDescriptorTable(3, whiteHandle);

        reinterpret_cast<CBPerObject*>(mCbMappedData)->ObjectColor = { 0.18f, 0.72f, 1.0f };
        D3D12_VERTEX_BUFFER_VIEW views[] = { mCubeVbView, mInstanceVbView };
        mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        mCommandList->IASetVertexBuffers(0, 2, views);
        mCommandList->IASetIndexBuffer(&mCubeIbView);
        mCommandList->DrawIndexedInstanced(36, mVisibleObjectCount, 0, 0, 0);
    }

    // ---------- Tessellation pass: displacement + normal map ----------
    mRenderingSystem.BeginTessellationPass(mCommandList.Get(), mWireframe);

    CD3DX12_GPU_DESCRIPTOR_HANDLE tessellationCbv(
        mSrvHeap->GetGPUDescriptorHandleForHeapStart(),
        kTessellationCbvIndex, mCbvSrvUavDescriptorSize);
    CD3DX12_GPU_DESCRIPTOR_HANDLE tessellationAlbedo(
        mSrvHeap->GetGPUDescriptorHandleForHeapStart(),
        kTessellationAlbedoIndex, mCbvSrvUavDescriptorSize);
    CD3DX12_GPU_DESCRIPTOR_HANDLE tessellationNormal(
        mSrvHeap->GetGPUDescriptorHandleForHeapStart(),
        kTessellationNormalIndex, mCbvSrvUavDescriptorSize);
    CD3DX12_GPU_DESCRIPTOR_HANDLE tessellationDisplacement(
        mSrvHeap->GetGPUDescriptorHandleForHeapStart(),
        kTessellationDisplacementIndex, mCbvSrvUavDescriptorSize);

    mCommandList->SetGraphicsRootDescriptorTable(0, tessellationCbv);
    mCommandList->SetGraphicsRootDescriptorTable(1, tessellationAlbedo);
    mCommandList->SetGraphicsRootDescriptorTable(2, tessellationNormal);
    mCommandList->SetGraphicsRootDescriptorTable(3, tessellationDisplacement);
    mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);
    mCommandList->IASetVertexBuffers(0, 1, &mTessellationVbView);
    mCommandList->IASetIndexBuffer(nullptr);
    mCommandList->DrawInstanced(4, 1, 0, 0);

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

void SponzaApp::BuildTessellationConstantBuffer()
{
    UINT cbSize = (sizeof(CBPerObject) + 255) & ~255;
    CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
    auto cbDesc = CD3DX12_RESOURCE_DESC::Buffer(cbSize);
    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&mTessellationConstantBuffer)));

    ThrowIfFailed(mTessellationConstantBuffer->Map(0, nullptr,
        reinterpret_cast<void**>(&mTessellationCbMappedData)));

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = mTessellationConstantBuffer->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = cbSize;
    CD3DX12_CPU_DESCRIPTOR_HANDLE h(
        mSrvHeap->GetCPUDescriptorHandleForHeapStart(),
        kTessellationCbvIndex, mCbvSrvUavDescriptorSize);
    md3dDevice->CreateConstantBufferView(&cbvDesc, h);
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

    mInstancedInputLayout = mInputLayout;
    mInstancedInputLayout.push_back(
        { "INSTANCE_DATA", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0,
          D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 });
}

void SponzaApp::BuildRenderingSystem()
{
    mRenderingSystem.Initialize(md3dDevice.Get(), mBackBufferFormat,
        mDepthStencilFormat, mInputLayout, mInstancedInputLayout, &mGBuffer);
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

void SponzaApp::LoadTessellationTextures()
{
    if (!LoadTexture("tessellation/bricks2.jpg", kTessellationAlbedoIndex)
        || !LoadTexture("tessellation/bricks2_normal.jpg", kTessellationNormalIndex)
        || !LoadTexture("tessellation/bricks2_disp.jpg", kTessellationDisplacementIndex))
    {
        throw std::runtime_error("Failed to load tessellation textures");
    }
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

void SponzaApp::ShootLight()
{
    const XMVECTOR direction = XMVector3Normalize(
        XMVectorSubtract(XMLoadFloat3(&mCameraTarget), XMLoadFloat3(&mEyePos)));

    XMFLOAT3 rayDirection;
    XMStoreFloat3(&rayDirection, direction);

    XMFLOAT3 hitPoint;
    if (!FindRayHit(mEyePos, rayDirection, hitPoint))
        return;

    if ((int)mShotLights.size() == kMaxShotLights)
        mShotLights.erase(mShotLights.begin());

    static const XMFLOAT3 colors[] =
    {
        { 1.0f, 0.20f, 0.10f }, { 0.15f, 0.45f, 1.0f },
        { 1.0f, 0.80f, 0.12f }, { 0.90f, 0.15f, 0.85f }
    };

    ShotLight shot;
    shot.Position = mEyePos;
    shot.Target = {
        hitPoint.x - rayDirection.x * 2.f,
        hitPoint.y - rayDirection.y * 2.f,
        hitPoint.z - rayDirection.z * 2.f
    };
    shot.Color = colors[mNextShotColor++ % _countof(colors)];
    mShotLights.push_back(shot);
}

void SponzaApp::UpdateShotLights(float deltaTime)
{
    for (ShotLight& shot : mShotLights)
    {
        if (shot.Stuck)
            continue;

        XMVECTOR toTarget = XMVectorSubtract(
            XMLoadFloat3(&shot.Target), XMLoadFloat3(&shot.Position));
        const float distance = XMVectorGetX(XMVector3Length(toTarget));
        const float step = shot.Speed * deltaTime;

        if (distance <= step)
        {
            shot.Position = shot.Target;
            shot.Stuck = true;
        }
        else
        {
            toTarget = XMVectorScale(XMVector3Normalize(toTarget), step);
            XMStoreFloat3(&shot.Position,
                XMVectorAdd(XMLoadFloat3(&shot.Position), toTarget));
        }
    }

    mLights.PointCount = (float)(kStaticPointLights + mShotLights.size());
    for (size_t i = 0; i < mShotLights.size(); ++i)
    {
        const ShotLight& shot = mShotLights[i];
        PointLight& light = mLights.Points[kStaticPointLights + i];
        light.Position = shot.Position;
        light.Radius = shot.Radius;
        light.Color = shot.Color;
        light.Intensity = shot.Intensity;
    }
}

void SponzaApp::UpdateCameraMovement(float deltaTime)
{
    const bool forwardPressed = (GetAsyncKeyState('W') & 0x8000) != 0;
    const bool backwardPressed = (GetAsyncKeyState('S') & 0x8000) != 0;
    const bool leftPressed = (GetAsyncKeyState('A') & 0x8000) != 0;
    const bool rightPressed = (GetAsyncKeyState('D') & 0x8000) != 0;

    if (!forwardPressed && !backwardPressed && !leftPressed && !rightPressed)
        return;

    XMVECTOR forward = XMVectorSubtract(
        XMLoadFloat3(&mCameraTarget), XMLoadFloat3(&mEyePos));
    forward = XMVector3Normalize(forward);

    const XMVECTOR up = XMVectorSet(0.f, 1.f, 0.f, 0.f);
    const XMVECTOR right = XMVector3Normalize(XMVector3Cross(up, forward));
    XMVECTOR movement = XMVectorZero();

    if (forwardPressed) movement = XMVectorAdd(movement, forward);
    if (backwardPressed) movement = XMVectorSubtract(movement, forward);
    if (rightPressed) movement = XMVectorAdd(movement, right);
    if (leftPressed) movement = XMVectorSubtract(movement, right);

    movement = XMVectorScale(XMVector3Normalize(movement), mCameraSpeed * deltaTime);
    XMStoreFloat3(&mCameraTarget,
        XMVectorAdd(XMLoadFloat3(&mCameraTarget), movement));
}

bool SponzaApp::FindRayHit(const XMFLOAT3& origin, const XMFLOAT3& direction,
    XMFLOAT3& hitPoint) const
{
    float closestDistance = (std::numeric_limits<float>::max)();
    bool found = false;

    for (const CollisionTriangle& triangle : mCollisionTriangles)
    {
        float distance = 0.f;
        if (RayTriangleIntersection(origin, direction, triangle, distance)
            && distance < closestDistance)
        {
            closestDistance = distance;
            found = true;
        }
    }

    if (!found)
        return false;

    hitPoint = {
        origin.x + direction.x * closestDistance,
        origin.y + direction.y * closestDistance,
        origin.z + direction.z * closestDistance
    };
    return true;
}

void SponzaApp::BuildTessellatedSurface()
{
    // Площадка пола внутри Sponza. X и Z задают её границы,
    // Y — высоту пола в координатах модели.
    const float extent = 700.f;
    const float height = 10.f;
    const Vertex vertices[4] =
    {
        { {-extent, height, -extent}, {0.f, 1.f, 0.f}, {0.f, 0.f} },
        { { extent, height, -extent}, {0.f, 1.f, 0.f}, {1.f, 0.f} },
        { {-extent, height,  extent}, {0.f, 1.f, 0.f}, {0.f, 1.f} },
        { { extent, height,  extent}, {0.f, 1.f, 0.f}, {1.f, 1.f} }
    };

    UploadBufferData(mTessellationVertexBuffer, mTessellationVertexUpload,
        vertices, sizeof(vertices), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

    mTessellationVbView.BufferLocation = mTessellationVertexBuffer->GetGPUVirtualAddress();
    mTessellationVbView.SizeInBytes = sizeof(vertices);
    mTessellationVbView.StrideInBytes = sizeof(Vertex);
}

void SponzaApp::BuildCubeGeometry()
{
    const Vertex vertices[] =
    {
        {{-0.5f,-0.5f,-0.5f},{ 0, 0,-1},{0,1}}, {{ 0.5f,-0.5f,-0.5f},{ 0, 0,-1},{1,1}},
        {{ 0.5f, 0.5f,-0.5f},{ 0, 0,-1},{1,0}}, {{-0.5f, 0.5f,-0.5f},{ 0, 0,-1},{0,0}},
        {{ 0.5f,-0.5f, 0.5f},{ 0, 0, 1},{0,1}}, {{-0.5f,-0.5f, 0.5f},{ 0, 0, 1},{1,1}},
        {{-0.5f, 0.5f, 0.5f},{ 0, 0, 1},{1,0}}, {{ 0.5f, 0.5f, 0.5f},{ 0, 0, 1},{0,0}},
        {{-0.5f, 0.5f,-0.5f},{ 0, 1, 0},{0,1}}, {{ 0.5f, 0.5f,-0.5f},{ 0, 1, 0},{1,1}},
        {{ 0.5f, 0.5f, 0.5f},{ 0, 1, 0},{1,0}}, {{-0.5f, 0.5f, 0.5f},{ 0, 1, 0},{0,0}},
        {{-0.5f,-0.5f, 0.5f},{ 0,-1, 0},{0,1}}, {{ 0.5f,-0.5f, 0.5f},{ 0,-1, 0},{1,1}},
        {{ 0.5f,-0.5f,-0.5f},{ 0,-1, 0},{1,0}}, {{-0.5f,-0.5f,-0.5f},{ 0,-1, 0},{0,0}},
        {{-0.5f,-0.5f, 0.5f},{-1, 0, 0},{0,1}}, {{-0.5f,-0.5f,-0.5f},{-1, 0, 0},{1,1}},
        {{-0.5f, 0.5f,-0.5f},{-1, 0, 0},{1,0}}, {{-0.5f, 0.5f, 0.5f},{-1, 0, 0},{0,0}},
        {{ 0.5f,-0.5f,-0.5f},{ 1, 0, 0},{0,1}}, {{ 0.5f,-0.5f, 0.5f},{ 1, 0, 0},{1,1}},
        {{ 0.5f, 0.5f, 0.5f},{ 1, 0, 0},{1,0}}, {{ 0.5f, 0.5f,-0.5f},{ 1, 0, 0},{0,0}},
    };
    const std::uint16_t indices[] =
    {
        0,1,2, 0,2,3, 4,5,6, 4,6,7, 8,9,10, 8,10,11,
        12,13,14, 12,14,15, 16,17,18, 16,18,19, 20,21,22, 20,22,23
    };

    UploadBufferData(mCubeVertexBuffer, mCubeVertexUpload, vertices, sizeof(vertices),
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    UploadBufferData(mCubeIndexBuffer, mCubeIndexUpload, indices, sizeof(indices),
        D3D12_RESOURCE_STATE_INDEX_BUFFER);

    mCubeVbView = { mCubeVertexBuffer->GetGPUVirtualAddress(), sizeof(vertices), sizeof(Vertex) };
    mCubeIbView.BufferLocation = mCubeIndexBuffer->GetGPUVirtualAddress();
    mCubeIbView.SizeInBytes = sizeof(indices);
    mCubeIbView.Format = DXGI_FORMAT_R16_UINT;
}

void SponzaApp::BuildSceneObjects()
{
    constexpr int side = 48;
    mSceneObjects.reserve(side * side);
    unsigned int randomState = 0x1234ABCDu;
    auto random01 = [&randomState]()
    {
        randomState = randomState * 1664525u + 1013904223u;
        return float((randomState >> 8) & 0x00FFFFFFu) / float(0x01000000u);
    };

    for (int z = 0; z < side; ++z)
    {
        for (int x = 0; x < side; ++x)
        {
            const float scale = 18.f + random01() * 34.f;
            const float px = (x - (side - 1) * 0.5f) * 175.f + (random01() - 0.5f) * 70.f;
            const float pz = (z - (side - 1) * 0.5f) * 175.f + (random01() - 0.5f) * 70.f;
            const float py = -70.f + scale * 0.5f;
            SceneObject object = {};
            object.Position = { px, py, pz };
            object.Scale = scale;
            object.Bounds = { object.Position, { scale * 0.5f, scale * 0.5f, scale * 0.5f } };
            mSceneObjects.push_back(object);
        }
    }
    mVisibleInstances.reserve(mSceneObjects.size());
}

void SponzaApp::BuildOctree()
{
    mOctreeRoot = std::make_unique<OctreeNode>();
    mOctreeRoot->Bounds = { { 0.f, 200.f, 0.f }, { 4400.f, 1000.f, 4400.f } };
    mOctreeRoot->ObjectIndices.resize(mSceneObjects.size());
    for (UINT i = 0; i < mSceneObjects.size(); ++i)
        mOctreeRoot->ObjectIndices[i] = i;
    SubdivideOctree(*mOctreeRoot, 0);
}

void SponzaApp::BuildInstanceBuffer()
{
    const UINT byteSize = static_cast<UINT>(sizeof(InstanceData) * mSceneObjects.size());
    CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
    auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(byteSize);
    ThrowIfFailed(md3dDevice->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
        &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&mInstanceBuffer)));
    ThrowIfFailed(mInstanceBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mInstanceMappedData)));
    mInstanceVbView.BufferLocation = mInstanceBuffer->GetGPUVirtualAddress();
    mInstanceVbView.SizeInBytes = byteSize;
    mInstanceVbView.StrideInBytes = sizeof(InstanceData);
}

std::array<FrustumPlane, 6> SponzaApp::ExtractFrustumPlanes(const XMMATRIX& viewProj) const
{
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, viewProj);
    auto makePlane = [](float x, float y, float z, float w)
    {
        const float length = sqrtf(x * x + y * y + z * z);
        return FrustumPlane{ { x / length, y / length, z / length }, w / length };
    };
    return
    {
        makePlane(m._11 + m._14, m._21 + m._24, m._31 + m._34, m._41 + m._44),
        makePlane(-m._11 + m._14, -m._21 + m._24, -m._31 + m._34, -m._41 + m._44),
        makePlane(m._12 + m._14, m._22 + m._24, m._32 + m._34, m._42 + m._44),
        makePlane(-m._12 + m._14, -m._22 + m._24, -m._32 + m._34, -m._42 + m._44),
        makePlane(m._13, m._23, m._33, m._43),
        makePlane(-m._13 + m._14, -m._23 + m._24, -m._33 + m._34, -m._43 + m._44)
    };
}

bool SponzaApp::IntersectsFrustum(const BoundingBox& box,
    const std::array<FrustumPlane, 6>& planes) const
{
    for (const FrustumPlane& plane : planes)
    {
        const float radius = fabsf(plane.Normal.x) * box.Extents.x
            + fabsf(plane.Normal.y) * box.Extents.y
            + fabsf(plane.Normal.z) * box.Extents.z;
        const float distance = plane.Normal.x * box.Center.x
            + plane.Normal.y * box.Center.y + plane.Normal.z * box.Center.z
            + plane.Distance;
        if (distance + radius < 0.f)
            return false;
    }
    return true;
}

void SponzaApp::SubdivideOctree(OctreeNode& node, int depth)
{
    if (node.ObjectIndices.size() <= 24 || depth >= 5)
        return;

    const XMFLOAT3 childExtents = { node.Bounds.Extents.x * 0.5f,
        node.Bounds.Extents.y * 0.5f, node.Bounds.Extents.z * 0.5f };
    for (int i = 0; i < 8; ++i)
    {
        const XMFLOAT3 center =
        {
            node.Bounds.Center.x + ((i & 1) ? childExtents.x : -childExtents.x),
            node.Bounds.Center.y + ((i & 2) ? childExtents.y : -childExtents.y),
            node.Bounds.Center.z + ((i & 4) ? childExtents.z : -childExtents.z)
        };
        node.Children[i] = std::make_unique<OctreeNode>();
        node.Children[i]->Bounds = { center, childExtents };
    }

    std::vector<UINT> remaining;
    for (UINT objectIndex : node.ObjectIndices)
    {
        const BoundingBox& box = mSceneObjects[objectIndex].Bounds;
        int childIndex = 0;
        const float minX = box.Center.x - box.Extents.x, maxX = box.Center.x + box.Extents.x;
        const float minY = box.Center.y - box.Extents.y, maxY = box.Center.y + box.Extents.y;
        const float minZ = box.Center.z - box.Extents.z, maxZ = box.Center.z + box.Extents.z;
        if (maxX <= node.Bounds.Center.x) {} else if (minX >= node.Bounds.Center.x) childIndex |= 1; else { remaining.push_back(objectIndex); continue; }
        if (maxY <= node.Bounds.Center.y) {} else if (minY >= node.Bounds.Center.y) childIndex |= 2; else { remaining.push_back(objectIndex); continue; }
        if (maxZ <= node.Bounds.Center.z) {} else if (minZ >= node.Bounds.Center.z) childIndex |= 4; else { remaining.push_back(objectIndex); continue; }
        node.Children[childIndex]->ObjectIndices.push_back(objectIndex);
    }
    node.ObjectIndices = std::move(remaining);
    for (auto& child : node.Children)
        if (!child->ObjectIndices.empty())
            SubdivideOctree(*child, depth + 1);
}

void SponzaApp::QueryOctree(const OctreeNode& node,
    const std::array<FrustumPlane, 6>& planes, std::vector<UINT>& visible) const
{
    if (!IntersectsFrustum(node.Bounds, planes))
        return;

    for (UINT objectIndex : node.ObjectIndices)
        if (IntersectsFrustum(mSceneObjects[objectIndex].Bounds, planes))
            visible.push_back(objectIndex);

    for (const auto& child : node.Children)
        if (child)
            QueryOctree(*child, planes, visible);
}

void SponzaApp::UpdateVisibleInstances(const XMMATRIX& viewProj)
{
    std::vector<UINT> visible;
    visible.reserve(mSceneObjects.size());
    if (!mFrustumCullingEnabled)
    {
        for (UINT i = 0; i < mSceneObjects.size(); ++i)
            visible.push_back(i);
    }
    else
    {
        const auto planes = ExtractFrustumPlanes(viewProj);
        if (mOctreeCullingEnabled)
            QueryOctree(*mOctreeRoot, planes, visible);
        else
            for (UINT i = 0; i < mSceneObjects.size(); ++i)
                if (IntersectsFrustum(mSceneObjects[i].Bounds, planes))
                    visible.push_back(i);
    }

    mVisibleInstances.clear();
    for (UINT objectIndex : visible)
    {
        const SceneObject& object = mSceneObjects[objectIndex];
        mVisibleInstances.push_back({ { object.Position.x, object.Position.y,
            object.Position.z, object.Scale } });
    }
    mVisibleObjectCount = static_cast<UINT>(mVisibleInstances.size());
    if (mVisibleObjectCount > 0)
        memcpy(mInstanceMappedData, mVisibleInstances.data(),
            sizeof(InstanceData) * mVisibleObjectCount);
}

void SponzaApp::UpdateWindowCaption()
{
    mMainWndCaption = L"Sponza - WASD: move | LMB: shoot | RMB: orbit | F: wireframe"
        L" | C: culling " + std::wstring(mFrustumCullingEnabled ? L"ON" : L"OFF")
        + L" | O: octree " + std::wstring(mOctreeCullingEnabled ? L"ON" : L"OFF")
        + L" | cubes: " + std::to_wstring(mVisibleObjectCount)
        + L"/" + std::to_wstring(mSceneObjects.size());
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

    mCollisionTriangles.clear();
    mCollisionTriangles.reserve(allIndices.size() / 3);
    for (size_t i = 0; i + 2 < allIndices.size(); i += 3)
    {
        mCollisionTriangles.push_back({
            allVerts[allIndices[i + 0]].Pos,
            allVerts[allIndices[i + 1]].Pos,
            allVerts[allIndices[i + 2]].Pos
        });
    }

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
