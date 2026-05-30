#include "D3DApp.h"
#include <cassert>
#include <stdexcept>
#include <windowsx.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "user32.lib")

D3DApp* D3DApp::mApp = nullptr;

static void ThrowIfFailed(HRESULT hr)
{
    if (FAILED(hr))
        throw std::runtime_error("HRESULT failed");
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    return D3DApp::GetApp()->MsgProc(hwnd, msg, wParam, lParam);
}

D3DApp::D3DApp(HINSTANCE hInstance)
    : mhAppInst(hInstance)
{
    assert(mApp == nullptr);
    mApp = this;
}

D3DApp::~D3DApp()
{
    if (md3dDevice != nullptr)
        FlushCommandQueue();
}

D3DApp* D3DApp::GetApp() { return mApp; }
HINSTANCE D3DApp::AppInst()     const { return mhAppInst; }
HWND      D3DApp::MainWnd()     const { return mhMainWnd; }
float     D3DApp::AspectRatio() const
{
    return (float)mClientWidth / (float)mClientHeight;
}

// =============================================================
int D3DApp::Run()
{
    MSG msg = {};
    mTimer.Reset();

    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else
        {
            mTimer.Tick();
            if (!mAppPaused)
            {
                CalculateFrameStats();
                Update(mTimer);
                Draw(mTimer);
            }
            else
            {
                Sleep(100);
            }
        }
    }

    return (int)msg.wParam;
}

// =============================================================
bool D3DApp::Initialize()
{
    if (!InitMainWindow())  return false;
    if (!InitDirect3D())    return false;
    OnResize();
    return true;
}

// =============================================================
LRESULT D3DApp::MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE)
        {
            mAppPaused = true;  mTimer.Stop();
        }
        else
        {
            mAppPaused = false; mTimer.Start();
        }
        return 0;

    case WM_SIZE:
        mClientWidth = LOWORD(lParam);
        mClientHeight = HIWORD(lParam);
        if (md3dDevice)
        {
            if (wParam == SIZE_MINIMIZED)
            {
                mAppPaused = true; mMinimized = true; mMaximized = false;
            }
            else if (wParam == SIZE_MAXIMIZED)
            {
                mAppPaused = false; mMinimized = false; mMaximized = true;
                OnResize();
            }
            else if (wParam == SIZE_RESTORED)
            {
                if (mMinimized) { mAppPaused = false; mMinimized = false; OnResize(); }
                else if (mMaximized) { mAppPaused = false; mMaximized = false; OnResize(); }
                else if (!mResizing) { OnResize(); }
            }
        }
        return 0;

    case WM_ENTERSIZEMOVE:
        mAppPaused = true; mResizing = true; mTimer.Stop();
        return 0;

    case WM_EXITSIZEMOVE:
        mAppPaused = false; mResizing = false; mTimer.Start();
        OnResize();
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_KEYUP:
        if (wParam == VK_ESCAPE)
            PostQuitMessage(0);
        // Все нажатия клавиш передаём в наследник
        OnKeyboardInput(wParam);
        return 0;

        // --- Мышь ---
    case WM_LBUTTONDOWN:
        OnMouseDown(wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_LBUTTONUP:
        OnMouseUp(wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_MOUSEMOVE:
        OnMouseMove(wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_MOUSEWHEEL:
        OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// =============================================================
void D3DApp::OnResize()
{
    assert(md3dDevice);
    assert(mSwapChain);
    assert(mDirectCmdListAlloc);

    FlushCommandQueue();

    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

    for (int i = 0; i < kSwapChainBufferCount; i++)
        mSwapChainBuffer[i].Reset();
    mDepthStencilBuffer.Reset();

    ThrowIfFailed(mSwapChain->ResizeBuffers(
        kSwapChainBufferCount,
        mClientWidth, mClientHeight,
        mBackBufferFormat,
        DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH));

    mCurrBackBuffer = 0;

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(
        mRtvHeap->GetCPUDescriptorHandleForHeapStart());

    for (int i = 0; i < kSwapChainBufferCount; i++)
    {
        ThrowIfFailed(mSwapChain->GetBuffer(
            i, IID_PPV_ARGS(&mSwapChainBuffer[i])));
        md3dDevice->CreateRenderTargetView(
            mSwapChainBuffer[i].Get(), nullptr, rtvHandle);
        rtvHandle.Offset(1, mRtvDescriptorSize);
    }

    D3D12_RESOURCE_DESC depthDesc = {};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = mClientWidth;
    depthDesc.Height = mClientHeight;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = mDepthStencilFormat;
    depthDesc.SampleDesc = { 1, 0 };
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE optClear = {};
    optClear.Format = mDepthStencilFormat;
    optClear.DepthStencil.Depth = 1.f;
    optClear.DepthStencil.Stencil = 0;

    CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE,
        &depthDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &optClear, IID_PPV_ARGS(&mDepthStencilBuffer)));

    md3dDevice->CreateDepthStencilView(
        mDepthStencilBuffer.Get(), nullptr, DepthStencilView());

    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(1, cmdsLists);
    FlushCommandQueue();

    mScreenViewport.TopLeftX = 0;
    mScreenViewport.TopLeftY = 0;
    mScreenViewport.Width = (float)mClientWidth;
    mScreenViewport.Height = (float)mClientHeight;
    mScreenViewport.MinDepth = 0.f;
    mScreenViewport.MaxDepth = 1.f;

    mScissorRect = { 0, 0, mClientWidth, mClientHeight };
}

// =============================================================
bool D3DApp::InitMainWindow()
{
    WNDCLASS wc = {};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = mhAppInst;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
    wc.lpszClassName = L"MainWnd";
    RegisterClass(&wc);

    RECT rc = { 0, 0, mClientWidth, mClientHeight };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, false);

    mhMainWnd = CreateWindow(
        L"MainWnd", mMainWndCaption.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, mhAppInst, nullptr);

    if (!mhMainWnd) return false;

    ShowWindow(mhMainWnd, SW_SHOW);
    UpdateWindow(mhMainWnd);
    return true;
}

// =============================================================
bool D3DApp::InitDirect3D()
{
#ifdef _DEBUG
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        debugController->EnableDebugLayer();
#endif

    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&mdxgiFactory)));
    ThrowIfFailed(D3D12CreateDevice(
        nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&md3dDevice)));

    ThrowIfFailed(md3dDevice->CreateFence(
        0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mFence)));

    mRtvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    mDsvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    mCbvSrvUavDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    CreateCommandObjects();
    CreateSwapChain();
    CreateRtvAndDsvDescriptorHeaps();

    return true;
}

// =============================================================
void D3DApp::CreateCommandObjects()
{
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ThrowIfFailed(md3dDevice->CreateCommandQueue(
        &queueDesc, IID_PPV_ARGS(&mCommandQueue)));

    ThrowIfFailed(md3dDevice->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&mDirectCmdListAlloc)));

    ThrowIfFailed(md3dDevice->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        mDirectCmdListAlloc.Get(), nullptr,
        IID_PPV_ARGS(&mCommandList)));

    mCommandList->Close();
}

// =============================================================
void D3DApp::CreateSwapChain()
{
    mSwapChain.Reset();

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc.Width = mClientWidth;
    sd.BufferDesc.Height = mClientHeight;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferDesc.Format = mBackBufferFormat;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kSwapChainBufferCount;
    sd.OutputWindow = mhMainWnd;
    sd.Windowed = true;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    ThrowIfFailed(mdxgiFactory->CreateSwapChain(
        mCommandQueue.Get(), &sd, &mSwapChain));
}

// =============================================================
void D3DApp::CreateRtvAndDsvDescriptorHeaps()
{
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = kSwapChainBufferCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
        &rtvHeapDesc, IID_PPV_ARGS(&mRtvHeap)));

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
        &dsvHeapDesc, IID_PPV_ARGS(&mDsvHeap)));
}

// =============================================================
void D3DApp::FlushCommandQueue()
{
    mCurrentFence++;
    ThrowIfFailed(mCommandQueue->Signal(mFence.Get(), mCurrentFence));

    if (mFence->GetCompletedValue() < mCurrentFence)
    {
        HANDLE eventHandle = CreateEventEx(nullptr, nullptr, false, EVENT_ALL_ACCESS);
        ThrowIfFailed(mFence->SetEventOnCompletion(mCurrentFence, eventHandle));
        WaitForSingleObject(eventHandle, INFINITE);
        CloseHandle(eventHandle);
    }
}

// =============================================================
ID3D12Resource* D3DApp::CurrentBackBuffer() const
{
    return mSwapChainBuffer[mCurrBackBuffer].Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE D3DApp::CurrentBackBufferView() const
{
    return CD3DX12_CPU_DESCRIPTOR_HANDLE(
        mRtvHeap->GetCPUDescriptorHandleForHeapStart(),
        mCurrBackBuffer, mRtvDescriptorSize);
}

D3D12_CPU_DESCRIPTOR_HANDLE D3DApp::DepthStencilView() const
{
    return mDsvHeap->GetCPUDescriptorHandleForHeapStart();
}

// =============================================================
void D3DApp::CalculateFrameStats()
{
    static int   frameCnt = 0;
    static float timeElapsed = 0.f;

    frameCnt++;
    if ((mTimer.TotalTime() - timeElapsed) >= 1.f)
    {
        float fps = (float)frameCnt;
        float mspf = 1000.f / fps;

        std::wstring windowText = mMainWndCaption +
            L"    fps: " + std::to_wstring((int)fps) +
            L"   ms: " + std::to_wstring(mspf) +
            L"   [F] Wireframe";

        SetWindowText(mhMainWnd, windowText.c_str());

        frameCnt = 0;
        timeElapsed += 1.f;
    }
}
