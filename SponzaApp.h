#pragma once

#include "D3DApp.h"
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <array>
#include "GBuffer.h"
#include "RenderingSystem.h"

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

struct CollisionTriangle
{
    XMFLOAT3 A;
    XMFLOAT3 B;
    XMFLOAT3 C;
};

struct ShotLight
{
    XMFLOAT3 Position;
    XMFLOAT3 Target;
    XMFLOAT3 Color;
    float Radius = 260.f;
    float Intensity = 12.f;
    float Speed = 1400.f;
    bool Stuck = false;
};

struct CBPerObject
{
    XMFLOAT4X4 World;
    XMFLOAT4X4 ViewProj;
    XMFLOAT3   ObjectColor; float Pad0;
    XMFLOAT2   UVScale;
    XMFLOAT2   UVOffset;
    XMFLOAT3   EyePos; float TessellationScale;
    float      UseNormalMap;
    float      DisplacementScale;
    XMFLOAT2   Pad1;
};

class SponzaApp : public D3DApp
{
public:
    SponzaApp(HINSTANCE hInstance);
    ~SponzaApp();

    bool Initialize() override;

    void OnMouseDown(WPARAM btnState, int x, int y);
    void OnMouseUp(WPARAM btnState, int x, int y);
    void OnMouseMove(WPARAM btnState, int x, int y);
    void OnMouseWheel(short delta);

    // Клавиатура — переключение wireframe
    void OnKeyboardInput(WPARAM key) override;

protected:
    void OnResize() override;
    void Update(const GameTimer& gt) override;
    void Draw(const GameTimer& gt)   override;

private:
    void BuildDescriptorHeap();
    void BuildConstantBuffer();
    void BuildTessellationConstantBuffer();
    void BuildShadersAndInputLayout();
    void BuildGeometry();
    void BuildTessellatedSurface();
    void BuildRenderingSystem();
    void BuildLights();

    void LoadModel(const std::string& objPath);
    bool LoadTexture(const std::string& path, int heapIndex);
    void CreateWhiteTexture();
    void LoadTessellationTextures();
    void ShootLight();
    void UpdateShotLights(float deltaTime);
    void UpdateCameraMovement(float deltaTime);
    bool FindRayHit(const XMFLOAT3& origin, const XMFLOAT3& direction,
        XMFLOAT3& hitPoint) const;

    void UploadBufferData(ComPtr<ID3D12Resource>& dest,
        ComPtr<ID3D12Resource>& upload,
        const void* data, UINT byteSize,
        D3D12_RESOURCE_STATES finalState);

private:
    static const int kMaxTextures = 128;
    static const int kStaticPointLights = 6;
    static const int kMaxShotLights = 16 - kStaticPointLights;
    static const int kGBufferSrvStart = 2 + kMaxTextures;
    static const int kLightCbvIndex = kGBufferSrvStart + GBuffer::BufferCount;
    static const int kTessellationAlbedoIndex = kLightCbvIndex + 1;
    static const int kTessellationNormalIndex = kTessellationAlbedoIndex + 1;
    static const int kTessellationDisplacementIndex = kTessellationNormalIndex + 1;
    static const int kTessellationCbvIndex = kTessellationDisplacementIndex + 1;
    static const int kTotalSrvSlots = kTessellationCbvIndex + 1;

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
    std::vector<CollisionTriangle> mCollisionTriangles;

    ComPtr<ID3D12Resource>   mTessellationVertexBuffer;
    ComPtr<ID3D12Resource>   mTessellationVertexUpload;
    D3D12_VERTEX_BUFFER_VIEW mTessellationVbView = {};

    ComPtr<ID3D12Resource> mConstantBuffer;
    BYTE* mCbMappedData = nullptr;
    ComPtr<ID3D12Resource> mTessellationConstantBuffer;
    BYTE* mTessellationCbMappedData = nullptr;

    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;
    GBuffer mGBuffer;
    RenderingSystem mRenderingSystem;
    CBFrameLights mLights = {};
    std::vector<ShotLight> mShotLights;
    int mNextShotColor = 0;

    // Флаг wireframe режима — переключается кнопкой F
    bool mWireframe = false;

    // Камера
    float    mYaw = 0.f;
    float    mPitch = -0.1f;
    float    mRadius = 1000.f;
    XMFLOAT3 mEyePos = { 0.f, 200.f, -1000.f };
    XMFLOAT3 mCameraTarget = { 0.f, 100.f, 0.f };
    float    mCameraSpeed = 650.f;

    // Мышь: ЛКМ стреляет, ПКМ вращает камеру.
    bool  mMouseDown = false;
    POINT mLastMouse = {};
    float mMouseSens = 0.005f;
    float mZoomSpeed = 50.f;

    // UV
    XMFLOAT2 mUVScale = { 2.f, 2.f };
    XMFLOAT2 mUVOffset = { 0.f, 0.f };
    float    mUVScrollSpeed = 0.05f;

    XMFLOAT4X4 mProj = {};
};
