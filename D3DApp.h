#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <string>
#include "d3dx12.h"
#include "GameTimer.h"

using Microsoft::WRL::ComPtr;

class D3DApp
{
public:
    D3DApp(HINSTANCE hInstance);
    D3DApp(const D3DApp&) = delete;
    D3DApp& operator=(const D3DApp&) = delete;
    virtual ~D3DApp();

    static D3DApp* GetApp();

    HINSTANCE AppInst()   const;
    HWND      MainWnd()   const;
    float     AspectRatio() const;

    int Run();

    virtual bool Initialize();
    virtual LRESULT MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

protected:
    virtual void OnResize();
    virtual void Update(const GameTimer& gt) = 0;
    virtual void Draw(const GameTimer& gt) = 0;

    // Мышь
    virtual void OnMouseDown(WPARAM btnState, int x, int y) {}
    virtual void OnMouseUp(WPARAM btnState, int x, int y) {}
    virtual void OnMouseMove(WPARAM btnState, int x, int y) {}
    virtual void OnMouseWheel(short delta) {}

    // Клавиатура
    virtual void OnKeyboardInput(WPARAM key) {}

    bool InitMainWindow();
    bool InitDirect3D();

    void CreateCommandObjects();
    void CreateSwapChain();
    void CreateRtvAndDsvDescriptorHeaps();

    void FlushCommandQueue();

    ID3D12Resource* CurrentBackBuffer() const;
    D3D12_CPU_DESCRIPTOR_HANDLE CurrentBackBufferView() const;
    D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView() const;

    void CalculateFrameStats();

protected:
    static D3DApp* mApp;

    HINSTANCE mhAppInst = nullptr;
    HWND      mhMainWnd = nullptr;

    bool mAppPaused = false;
    bool mMinimized = false;
    bool mMaximized = false;
    bool mResizing = false;

    GameTimer mTimer;

    ComPtr<IDXGIFactory4>             mdxgiFactory;
    ComPtr<ID3D12Device>              md3dDevice;
    ComPtr<ID3D12Fence>               mFence;
    UINT64                            mCurrentFence = 0;

    ComPtr<ID3D12CommandQueue>        mCommandQueue;
    ComPtr<ID3D12CommandAllocator>    mDirectCmdListAlloc;
    ComPtr<ID3D12GraphicsCommandList> mCommandList;

    static const int kSwapChainBufferCount = 2;
    int              mCurrBackBuffer = 0;

    ComPtr<IDXGISwapChain> mSwapChain;
    ComPtr<ID3D12Resource> mSwapChainBuffer[kSwapChainBufferCount];
    ComPtr<ID3D12Resource> mDepthStencilBuffer;

    ComPtr<ID3D12DescriptorHeap> mRtvHeap;
    ComPtr<ID3D12DescriptorHeap> mDsvHeap;

    D3D12_VIEWPORT mScreenViewport = {};
    D3D12_RECT     mScissorRect = {};

    UINT mRtvDescriptorSize = 0;
    UINT mDsvDescriptorSize = 0;
    UINT mCbvSrvUavDescriptorSize = 0;

    std::wstring mMainWndCaption = L"Sponza DX12";

    DXGI_FORMAT mBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_FORMAT mDepthStencilFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

    int mClientWidth = 1280;
    int mClientHeight = 720;
};
