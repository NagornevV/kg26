#pragma once

#include "D3DApp.h"
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <vector>
#include <string>
#include <unordered_map>

using namespace DirectX;

struct Vertex
{
    XMFLOAT3 Pos;
    XMFLOAT3 Normal;
    XMFLOAT2 TexC;
};

struct SubMesh
{
    UINT     IndexStart;
    UINT     IndexCount;
    XMFLOAT3 DiffuseColor;
    int      TextureIndex;
};

struct CBPerObject
{
    XMFLOAT4X4 World;
    XMFLOAT4X4 ViewProj;
    XMFLOAT3   LightDir;    float Pad0;
    XMFLOAT3   LightColor;  float Pad1;
    XMFLOAT3   EyePos;      float Pad2;
    XMFLOAT3   ObjectColor; float Pad3;
    XMFLOAT2   UVScale;
    XMFLOAT2   UVOffset;
};

class SponzaApp : public D3DApp
{
public:
    SponzaApp(HINSTANCE hInstance);
    ~SponzaApp();

    bool Initialize() override;

    // Вызываются из MsgProc
    void OnMouseDown(WPARAM btnState, int x, int y);
    void OnMouseUp(WPARAM btnState, int x, int y);
    void OnMouseMove(WPARAM btnState, int x, int y);
    void OnMouseWheel(short delta);

protected:
    void OnResize() override;
    void Update(const GameTimer& gt) override;
    void Draw(const GameTimer& gt) override;

private:
    void BuildDescriptorHeap();
    void BuildConstantBuffer();
    void BuildRootSignature();
    void BuildShadersAndInputLayout();
    void BuildGeometry();
    void BuildPSO();

    void LoadModel(const std::string& objPath);
    bool LoadTexture(const std::string& path, int heapIndex);
    void CreateWhiteTexture();

    void UploadBufferData(ComPtr<ID3D12Resource>& dest,
                          ComPtr<ID3D12Resource>& upload,
                          const void* data, UINT byteSize,
                          D3D12_RESOURCE_STATES finalState);

private:
    static const int kMaxTextures = 128;
    ComPtr<ID3D12DescriptorHeap> mSrvHeap;

    std::vector<ComPtr<ID3D12Resource>>  mTextures;
    std::vector<ComPtr<ID3D12Resource>>  mTextureUploads;
    std::unordered_map<std::string, int> mTextureMap;
    int mNextTexSlot = 2;

    ComPtr<ID3D12Resource>   mVertexBuffer;
    ComPtr<ID3D12Resource>   mIndexBuffer;
    ComPtr<ID3D12Resource>   mVertexUpload;
    ComPtr<ID3D12Resource>   mIndexUpload;
    D3D12_VERTEX_BUFFER_VIEW mVbView = {};
    D3D12_INDEX_BUFFER_VIEW  mIbView = {};
    std::vector<SubMesh>     mSubMeshes;

    ComPtr<ID3D12Resource> mConstantBuffer;
    BYTE*                  mCbMappedData = nullptr;

    ComPtr<ID3D12RootSignature>           mRootSignature;
    ComPtr<ID3D12PipelineState>           mPSO;
    ComPtr<ID3DBlob>                      mVsByteCode;
    ComPtr<ID3DBlob>                      mPsByteCode;
    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;

    // --- Камера ---
    float    mYaw    =  0.f;
    float    mPitch  = -0.1f;
    float    mRadius =  1000.f;  // расстояние от центра (колесико)
    XMFLOAT3 mEyePos = { 0.f, 200.f, -1000.f };

    // --- Мышь ---
    bool  mMouseDown = false;
    POINT mLastMouse = {};
    float mMouseSens = 0.005f;
    float mZoomSpeed = 50.f;

    // --- UV ---
    XMFLOAT2 mUVScale       = { 2.f, 2.f };
    XMFLOAT2 mUVOffset      = { 0.f, 0.f };
    float    mUVScrollSpeed = 0.05f;

    XMFLOAT4X4 mProj = {};
};
