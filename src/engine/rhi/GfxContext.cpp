#include "engine/rhi/GfxContext.h"

#include "engine/platform/FrameDiagnostics.h"
#include "engine/platform/EngineDiagnostics.h"
#include "engine/platform/EngineLog.h"
#include "engine/rendering/DxrCapabilities.h"
#include "engine/rendering/Framebuffer.h"
#include "engine/rhi/d3d12/D3D12Throw.h"
#include "engine/rhi/d3d12/FixedDescriptorHeap.h"

#include <imgui.h>
#include <imgui_impl_dx12.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <D3D12MemAlloc.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <algorithm>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
    void PumpWindowEvents(GLFWwindow* window)
    {
        if (window != nullptr)
        {
            glfwPollEvents();
        }
    }

    constexpr std::uint32_t SrvDescriptorCount = 8192;
    // Per-frame draw descriptor tables (copied texture slots) live below offscreen SRVs.
    constexpr std::uint32_t OffscreenSrvDescriptorStart = 4096;
    constexpr std::uint32_t OffscreenSrvDescriptorCount = SrvDescriptorCount - OffscreenSrvDescriptorStart;
    constexpr std::uint32_t OffscreenRtvCount = 256;
    constexpr std::uint32_t OffscreenDsvCount = 64;

    constexpr std::uint32_t TransientUploadCapacityBytes = 64u * 1024u * 1024u;

    struct FrameContext
    {
        ComPtr<ID3D12CommandAllocator> CommandAllocator;
        ComPtr<ID3D12Resource> RenderTarget;
        D3D12_CPU_DESCRIPTOR_HANDLE RtvCpuHandle{};
        std::uint64_t FenceValue = 0;
    };

    struct TransientUploadArena
    {
        ComPtr<ID3D12Resource> Resource;
        D3D12MA::Allocation* Allocation = nullptr;
        void* Mapped = nullptr;
        std::uint32_t Offset = 0;
    };
}

namespace GfxContextDetail
{
    void SetGpuAllocationError(const char* message)
    {
        EngineDiagnostics::SetLastGpuAllocationError(
            message != nullptr ? message : "unknown GPU allocation error");
    }

    std::string FormatHresult(const HRESULT hr)
    {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<unsigned>(hr));
        return buffer;
    }

    std::string WideToUtf8(const wchar_t* wideText)
    {
        if (wideText == nullptr || wideText[0] == L'\0')
        {
            return {};
        }

        const int requiredSize = WideCharToMultiByte(
            CP_UTF8,
            0,
            wideText,
            -1,
            nullptr,
            0,
            nullptr,
            nullptr);
        if (requiredSize <= 0)
        {
            return {};
        }

        std::string utf8(static_cast<std::size_t>(requiredSize - 1), '\0');
        WideCharToMultiByte(
            CP_UTF8,
            0,
            wideText,
            -1,
            utf8.data(),
            requiredSize,
            nullptr,
            nullptr);
        return utf8;
    }
}

struct GfxContext::Impl
{
    HWND Hwnd = nullptr;

    ComPtr<IDXGIFactory6> Factory;
    ComPtr<IDXGIAdapter1> Adapter;
    ComPtr<ID3D12Device> Device;
    ComPtr<ID3D12CommandQueue> CommandQueue;
    ComPtr<ID3D12CommandQueue> ImGuiUploadQueue;
    ComPtr<ID3D12GraphicsCommandList> CommandList;
    ComPtr<IDXGISwapChain3> SwapChain;
    ComPtr<ID3D12Fence> Fence;
    HANDLE FenceEvent = nullptr;

    ComPtr<ID3D12DescriptorHeap> RtvHeap;
    ComPtr<ID3D12DescriptorHeap> OffscreenRtvHeap;
    ComPtr<ID3D12DescriptorHeap> OffscreenDsvHeap;
    ComPtr<ID3D12DescriptorHeap> SrvHeap;
    std::uint32_t RtvDescriptorSize = 0;
    std::uint32_t DsvDescriptorSize = 0;
    std::uint32_t SrvDescriptorSize = 0;

    D3D12MA::Allocator* MemoryAllocator = nullptr;

    std::array<FrameContext, GfxContext::FrameCount> Frames{};
    std::array<TransientUploadArena, GfxContext::FrameCount> TransientUploadArenas{};
    FixedDescriptorHeap SrvAllocator;
    FixedDescriptorHeap OffscreenRtvAllocator;
    FixedDescriptorHeap OffscreenDsvAllocator;

    int ImmediateUploadDepth = 0;

    std::uint32_t DrawSrvTableNextIndex = GfxContext::DrawTextureDescriptorStart;
};

static void ImGuiAllocSrvDescriptor(
    ImGui_ImplDX12_InitInfo* info,
    D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu_handle,
    D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu_handle)
{
    auto* context = static_cast<GfxContext*>(info->UserData);
    context->AllocSrvDescriptorForImGui(out_cpu_handle, out_gpu_handle);
}

static void ImGuiFreeSrvDescriptor(
    ImGui_ImplDX12_InitInfo* info,
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
    D3D12_GPU_DESCRIPTOR_HANDLE /*gpu_handle*/)
{
    auto* context = static_cast<GfxContext*>(info->UserData);
    context->FreeSrvDescriptorFromCpuHandle(cpu_handle.ptr);
}

GfxContext& GfxContext::Get()
{
    static GfxContext instance;
    return instance;
}

bool GfxContext::Initialize(GLFWwindow* window, int width, int height)
{
    if (m_impl != nullptr)
    {
        return true;
    }

    m_window = window;
    m_width = width;
    m_height = height;
    m_impl = new Impl();
    m_impl->Hwnd = glfwGetWin32Window(window);

    UINT factoryFlags = 0;
#if defined(_DEBUG) && defined(GAME_ENGINE_D3D12_DEBUG_LAYER)
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif

    ThrowIfFailed(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_impl->Factory)), "CreateDXGIFactory2 failed");

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT adapterIndex = 0;
         m_impl->Factory->EnumAdapterByGpuPreference(
             adapterIndex,
             DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
             IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
         ++adapterIndex)
    {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        {
            continue;
        }

        const D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_12_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        for (const D3D_FEATURE_LEVEL featureLevel : featureLevels)
        {
            if (SUCCEEDED(D3D12CreateDevice(
                    adapter.Get(), featureLevel, IID_PPV_ARGS(&m_impl->Device))))
            {
                m_impl->Adapter = adapter;
                break;
            }
        }
        if (m_impl->Device != nullptr)
        {
            break;
        }
    }

    if (m_impl->Device == nullptr)
    {
        ThrowIfFailed(m_impl->Factory->EnumAdapters1(0, &adapter), "EnumAdapters1 failed");
        const D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_12_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        HRESULT createResult = E_FAIL;
        for (const D3D_FEATURE_LEVEL featureLevel : featureLevels)
        {
            createResult =
                D3D12CreateDevice(adapter.Get(), featureLevel, IID_PPV_ARGS(&m_impl->Device));
            if (SUCCEEDED(createResult))
            {
                break;
            }
        }
        ThrowIfFailed(createResult, "D3D12CreateDevice failed");
        m_impl->Adapter = adapter;
    }

    auto queryMsaaSupport = [](ID3D12Device* device, const DXGI_FORMAT format, const UINT sampleCount) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels{};
        levels.Format = format;
        levels.SampleCount = sampleCount;
        return SUCCEEDED(device->CheckFeatureSupport(
                   D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                   &levels,
                   sizeof(levels)))
            && levels.NumQualityLevels > 0;
    };

    const DXGI_FORMAT msaaFormats[] = {
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R16G16_FLOAT,
        DXGI_FORMAT_D24_UNORM_S8_UINT,
    };
    const int msaaSampleCounts[] = {2, 4, 8};
    m_supportedMsaaSampleCountsMask = 0;
    for (const int sampleCount : msaaSampleCounts)
    {
        bool supported = true;
        for (const DXGI_FORMAT format : msaaFormats)
        {
            if (!queryMsaaSupport(m_impl->Device.Get(), format, static_cast<UINT>(sampleCount)))
            {
                supported = false;
                break;
            }
        }

        if (supported)
        {
            m_supportedMsaaSampleCountsMask |= static_cast<std::uint8_t>(1u << sampleCount);
        }
    }
    m_activeMsaaSampleCount = 1;

    {
        DXGI_ADAPTER_DESC1 adapterDesc{};
        m_impl->Adapter->GetDesc1(&adapterDesc);
        m_adapterDescription = GfxContextDetail::WideToUtf8(adapterDesc.Description);

        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
        if (SUCCEEDED(m_impl->Device->CheckFeatureSupport(
                D3D12_FEATURE_D3D12_OPTIONS5,
                &options5,
                sizeof(options5))))
        {
            m_raytracingTier = static_cast<int>(options5.RaytracingTier);
        }
        else
        {
            m_raytracingTier = 0;
        }

        EngineLog::Info(
            "gfx",
            "Adapter: " + m_adapterDescription + " | Ray tracing tier: "
                + GetRaytracingTierLabel(m_raytracingTier));
        if (m_raytracingTier == 0)
        {
            EngineLog::Warn(
                "gfx",
                "Ray tracing is not supported on this GPU or driver. DXR features will be disabled.");
        }
    }

    D3D12MA::ALLOCATOR_DESC allocatorDesc{};
    allocatorDesc.pAdapter = m_impl->Adapter.Get();
    allocatorDesc.pDevice = m_impl->Device.Get();
    if (FAILED(D3D12MA::CreateAllocator(&allocatorDesc, &m_impl->MemoryAllocator)))
    {
        throw std::runtime_error("D3D12MA::CreateAllocator failed");
    }

    for (std::uint32_t frameIndex = 0; frameIndex < FrameCount; ++frameIndex)
    {
        TransientUploadArena& arena = m_impl->TransientUploadArenas[frameIndex];

        D3D12_RESOURCE_DESC uploadDesc{};
        uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDesc.Width = TransientUploadCapacityBytes;
        uploadDesc.Height = 1;
        uploadDesc.DepthOrArraySize = 1;
        uploadDesc.MipLevels = 1;
        uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
        uploadDesc.SampleDesc.Count = 1;
        uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        D3D12MA::ALLOCATION_DESC uploadAllocationDesc{};
        uploadAllocationDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;

        ID3D12Resource* uploadResource = nullptr;
        D3D12MA::Allocation* uploadAllocation = nullptr;
        if (FAILED(m_impl->MemoryAllocator->CreateResource(
                &uploadAllocationDesc,
                &uploadDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                &uploadAllocation,
                IID_PPV_ARGS(&uploadResource))))
        {
            throw std::runtime_error("Failed to create transient upload arena");
        }

        void* mapped = nullptr;
        if (FAILED(uploadResource->Map(0, nullptr, &mapped)))
        {
            uploadAllocation->Release();
            uploadResource->Release();
            throw std::runtime_error("Failed to map transient upload arena");
        }

        arena.Resource.Attach(uploadResource);
        arena.Allocation = uploadAllocation;
        arena.Mapped = mapped;
        arena.Offset = 0;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(
        m_impl->Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_impl->CommandQueue)),
        "CreateCommandQueue failed");

    ThrowIfFailed(
        m_impl->Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_impl->ImGuiUploadQueue)),
        "Create ImGui upload command queue failed");

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
    swapChainDesc.Width = static_cast<UINT>(width);
    swapChainDesc.Height = static_cast<UINT>(height);
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = FrameCount;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> swapChain1;
    ThrowIfFailed(
        m_impl->Factory->CreateSwapChainForHwnd(
            m_impl->CommandQueue.Get(),
            m_impl->Hwnd,
            &swapChainDesc,
            nullptr,
            nullptr,
            &swapChain1),
        "CreateSwapChainForHwnd failed");
    ThrowIfFailed(m_impl->Factory->MakeWindowAssociation(m_impl->Hwnd, DXGI_MWA_NO_ALT_ENTER), "MakeWindowAssociation failed");
    ThrowIfFailed(swapChain1.As(&m_impl->SwapChain), "SwapChain QueryInterface failed");

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.NumDescriptors = FrameCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(m_impl->Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_impl->RtvHeap)), "Create RTV heap failed");
    m_impl->RtvDescriptorSize = m_impl->Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC offscreenRtvHeapDesc{};
    offscreenRtvHeapDesc.NumDescriptors = OffscreenRtvCount;
    offscreenRtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(
        m_impl->Device->CreateDescriptorHeap(&offscreenRtvHeapDesc, IID_PPV_ARGS(&m_impl->OffscreenRtvHeap)),
        "Create offscreen RTV heap failed");
    m_impl->OffscreenRtvAllocator = FixedDescriptorHeap(OffscreenRtvCount);

    D3D12_DESCRIPTOR_HEAP_DESC offscreenDsvHeapDesc{};
    offscreenDsvHeapDesc.NumDescriptors = OffscreenDsvCount;
    offscreenDsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ThrowIfFailed(
        m_impl->Device->CreateDescriptorHeap(&offscreenDsvHeapDesc, IID_PPV_ARGS(&m_impl->OffscreenDsvHeap)),
        "Create offscreen DSV heap failed");
    m_impl->DsvDescriptorSize = m_impl->Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    m_impl->OffscreenDsvAllocator = FixedDescriptorHeap(OffscreenDsvCount);

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
    srvHeapDesc.NumDescriptors = SrvDescriptorCount;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_impl->Device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_impl->SrvHeap)), "Create SRV heap failed");
    m_impl->SrvDescriptorSize = m_impl->Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_impl->SrvAllocator = FixedDescriptorHeap(OffscreenSrvDescriptorCount);
    m_impl->SrvAllocator.SetIndexOffset(OffscreenSrvDescriptorStart);

    for (std::uint32_t frameIndex = 0; frameIndex < FrameCount; ++frameIndex)
    {
        ThrowIfFailed(
            m_impl->Device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&m_impl->Frames[frameIndex].CommandAllocator)),
            "CreateCommandAllocator failed");
        m_impl->Frames[frameIndex].FenceValue = 0;
    }

    ThrowIfFailed(
        m_impl->Device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_impl->Frames[0].CommandAllocator.Get(),
            nullptr,
            IID_PPV_ARGS(&m_impl->CommandList)),
        "CreateCommandList failed");
    ThrowIfFailed(m_impl->CommandList->Close(), "Close initial command list failed");

    ThrowIfFailed(m_impl->Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_impl->Fence)), "CreateFence failed");
    m_impl->FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (m_impl->FenceEvent == nullptr)
    {
        throw std::runtime_error("CreateEvent failed");
    }

    CreateRenderTargets();

    ImGui_ImplDX12_InitInfo imguiInit{};
    imguiInit.Device = m_impl->Device.Get();
    imguiInit.CommandQueue = m_impl->ImGuiUploadQueue.Get();
    imguiInit.NumFramesInFlight = static_cast<int>(FrameCount);
    imguiInit.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    imguiInit.SrvDescriptorHeap = m_impl->SrvHeap.Get();
    imguiInit.UserData = this;
    imguiInit.SrvDescriptorAllocFn = ImGuiAllocSrvDescriptor;
    imguiInit.SrvDescriptorFreeFn = ImGuiFreeSrvDescriptor;

    if (!ImGui_ImplDX12_Init(&imguiInit))
    {
        throw std::runtime_error("ImGui_ImplDX12_Init failed");
    }

    return true;
}

void GfxContext::Shutdown()
{
    if (m_impl == nullptr)
    {
        return;
    }

    WaitForGpu();
    ReleaseRenderTargets();

    for (TransientUploadArena& arena : m_impl->TransientUploadArenas)
    {
        if (arena.Resource != nullptr)
        {
            arena.Resource->Unmap(0, nullptr);
        }

        if (arena.Allocation != nullptr)
        {
            arena.Allocation->Release();
            arena.Allocation = nullptr;
        }

        arena.Resource.Reset();
        arena.Mapped = nullptr;
        arena.Offset = 0;
    }

    if (m_impl->MemoryAllocator != nullptr)
    {
        m_impl->MemoryAllocator->Release();
        m_impl->MemoryAllocator = nullptr;
    }

    if (m_impl->FenceEvent != nullptr)
    {
        CloseHandle(m_impl->FenceEvent);
        m_impl->FenceEvent = nullptr;
    }

    delete m_impl;
    m_impl = nullptr;
    m_window = nullptr;
    m_frameIndex = 0;
    m_fenceValues[0] = 0;
    m_fenceValues[1] = 0;
    m_boundOutputFramebuffer = nullptr;
    m_frameRecording = false;
    m_frameCommandsSubmitted = false;
}

void GfxContext::ResizeInternal(const int width, const int height)
{
    WaitForGpu();
    ReleaseRenderTargets();

    ThrowIfFailed(
        m_impl->SwapChain->ResizeBuffers(FrameCount, width, height, DXGI_FORMAT_UNKNOWN, 0),
        "ResizeBuffers failed");
    m_width = width;
    m_height = height;

    for (FrameContext& frame : m_impl->Frames)
    {
        frame.FenceValue = 0;
    }

    m_fenceValues[0] = 0;
    m_fenceValues[1] = 0;
    m_submissionFenceValue = 0;
    m_frameRecording = false;
    m_frameCommandsSubmitted = false;

    CreateRenderTargets();
}

void GfxContext::ProcessPendingResize()
{
    if (m_impl == nullptr || m_frameRecording)
    {
        return;
    }

    if (m_pendingResizeWidth <= 0 || m_pendingResizeHeight <= 0)
    {
        return;
    }

    const int width = m_pendingResizeWidth;
    const int height = m_pendingResizeHeight;
    m_pendingResizeWidth = 0;
    m_pendingResizeHeight = 0;

    if (width == m_width && height == m_height)
    {
        return;
    }

    ResizeInternal(width, height);
}

void GfxContext::Resize(int width, int height)
{
    if (m_impl == nullptr || width <= 0 || height <= 0)
    {
        return;
    }

    if (width == m_width && height == m_height)
    {
        m_pendingResizeWidth = 0;
        m_pendingResizeHeight = 0;
        return;
    }

    if (m_frameRecording)
    {
        m_pendingResizeWidth = width;
        m_pendingResizeHeight = height;
        return;
    }

    ResizeInternal(width, height);
}

void GfxContext::BeginFrame()
{
    if (m_impl == nullptr || m_width <= 0 || m_height <= 0)
    {
        return;
    }

    std::string deviceRemovedReason;
    if (IsDeviceRemoved(&deviceRemovedReason))
    {
        throw std::runtime_error("D3D12 device removed before frame begin: " + deviceRemovedReason);
    }

    if (m_frameRecording)
    {
        CancelFrame();
    }

    ProcessPendingResize();
    FrameContext& frame = m_impl->Frames[m_frameIndex];
    FrameDiagnostics::LogPhase("BeginFrame-wait");
    WaitForFenceValue(frame.FenceValue);

    const HRESULT allocatorResetResult = frame.CommandAllocator->Reset();
    if (FAILED(allocatorResetResult))
    {
        std::string message = "CommandAllocator reset failed (HRESULT=0x"
            + std::to_string(static_cast<unsigned long>(allocatorResetResult)) + ")";
        if (IsDeviceRemoved(&deviceRemovedReason))
        {
            message += "; device removed: " + deviceRemovedReason;
        }

        throw std::runtime_error(message);
    }

    const HRESULT commandListResetResult =
        m_impl->CommandList->Reset(frame.CommandAllocator.Get(), nullptr);
    if (FAILED(commandListResetResult))
    {
        std::string message = "CommandList reset failed (HRESULT=0x"
            + std::to_string(static_cast<unsigned long>(commandListResetResult)) + ")";
        if (IsDeviceRemoved(&deviceRemovedReason))
        {
            message += "; device removed: " + deviceRemovedReason;
        }

        throw std::runtime_error(message);
    }

    m_impl->TransientUploadArenas[m_frameIndex].Offset = 0;
    m_impl->DrawSrvTableNextIndex = DrawTextureDescriptorStart;
    EngineDiagnostics::ClearLastGpuAllocationError();
    m_frameCommandsSubmitted = false;

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = frame.RenderTarget.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_impl->CommandList->ResourceBarrier(1, &barrier);

    const float clearColor[] = {0.08f, 0.09f, 0.15f, 1.0f};
    m_impl->CommandList->ClearRenderTargetView(frame.RtvCpuHandle, clearColor, 0, nullptr);
    m_impl->CommandList->OMSetRenderTargets(1, &frame.RtvCpuHandle, FALSE, nullptr);

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(m_width);
    viewport.Height = static_cast<float>(m_height);
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor{0, 0, m_width, m_height};
    m_impl->CommandList->RSSetViewports(1, &viewport);
    m_impl->CommandList->RSSetScissorRects(1, &scissor);

    ID3D12DescriptorHeap* heaps[] = {m_impl->SrvHeap.Get()};
    m_impl->CommandList->SetDescriptorHeaps(1, heaps);
    m_frameRecording = true;
}

void GfxContext::CancelFrame()
{
    if (m_impl == nullptr || !m_frameRecording)
    {
        return;
    }

    FrameContext& frame = m_impl->Frames[m_frameIndex];
    auto* commandList = m_impl->CommandList.Get();
    if (!m_frameCommandsSubmitted && commandList != nullptr)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = frame.RenderTarget.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        commandList->ResourceBarrier(1, &barrier);

        const HRESULT closeResult = commandList->Close();
        if (SUCCEEDED(closeResult))
        {
            ID3D12CommandList* commandLists[] = {commandList};
            m_impl->CommandQueue->ExecuteCommandLists(1, commandLists);
            SignalFrameSubmission();
            m_frameCommandsSubmitted = true;
        }
    }

    if (m_frameCommandsSubmitted)
    {
        WaitForFenceValue(frame.FenceValue);
    }

    m_frameRecording = false;
    m_frameCommandsSubmitted = false;
    m_boundOutputFramebuffer = nullptr;
    m_impl->DrawSrvTableNextIndex = DrawTextureDescriptorStart;
}

void GfxContext::SubmitCommandList()
{
    if (m_impl == nullptr)
    {
        return;
    }

    FrameContext& frame = m_impl->Frames[m_frameIndex];
    auto* commandList = m_impl->CommandList.Get();

    // BeginFrame() transitions the swapchain buffer to RENDER_TARGET. Offscreen-only
    // direct-submit paths must restore PRESENT before the next BeginFrame().
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = frame.RenderTarget.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    commandList->ResourceBarrier(1, &barrier);

    ThrowIfFailed(m_impl->CommandList->Close(), "CommandList close failed");

    ID3D12CommandList* commandLists[] = {m_impl->CommandList.Get()};
    m_impl->CommandQueue->ExecuteCommandLists(1, commandLists);
    SignalFrameSubmission();
    m_frameCommandsSubmitted = true;
    m_frameRecording = false;
    AdvanceSwapchainFrameIndex();
}

void GfxContext::EndFrame()
{
    if (m_impl == nullptr)
    {
        return;
    }

    if (!m_frameRecording || m_width <= 0 || m_height <= 0)
    {
        CancelFrame();
        return;
    }

    FrameContext& frame = m_impl->Frames[m_frameIndex];
    auto* commandList = m_impl->CommandList.Get();

    BindSwapChainRenderTarget(false);

    if (ImDrawData* drawData = ImGui::GetDrawData())
    {
        RenderImGui(drawData);
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = frame.RenderTarget.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    commandList->ResourceBarrier(1, &barrier);

    ThrowIfFailed(m_impl->CommandList->Close(), "CommandList close failed");

    ID3D12CommandList* commandLists[] = {m_impl->CommandList.Get()};
    m_impl->CommandQueue->ExecuteCommandLists(1, commandLists);
    SignalFrameSubmission();
    m_frameCommandsSubmitted = true;

    PumpWindowEvents(m_window);

    const HRESULT presentResult = m_impl->SwapChain->Present(1, 0);

    m_frameRecording = false;
    m_boundOutputFramebuffer = nullptr;

    if (FAILED(presentResult))
    {
        std::string message =
            "Present failed (HRESULT=" + GfxContextDetail::FormatHresult(presentResult) + ")";
        std::string deviceRemovedReason;
        if (IsDeviceRemoved(&deviceRemovedReason))
        {
            message += "; device removed: " + deviceRemovedReason;
        }
        GfxContextDetail::SetGpuAllocationError(message.c_str());
        EngineLog::Error("gfx", message);
        m_frameCommandsSubmitted = false;
        throw std::runtime_error(message);
    }

    m_frameCommandsSubmitted = false;
    AdvanceSwapchainFrameIndex();
    ProcessPendingResize();
}

void* GfxContext::GetDevice() const
{
    return m_impl != nullptr ? m_impl->Device.Get() : nullptr;
}

void* GfxContext::GetCommandList() const
{
    return m_impl != nullptr ? m_impl->CommandList.Get() : nullptr;
}

void* GfxContext::GetSrvHeap() const
{
    return m_impl != nullptr ? m_impl->SrvHeap.Get() : nullptr;
}

D3D12MA::Allocator* GfxContext::GetMemoryAllocator() const
{
    return m_impl != nullptr ? m_impl->MemoryAllocator : nullptr;
}

std::uint32_t GfxContext::GetSrvDescriptorSize() const
{
    return m_impl != nullptr ? m_impl->SrvDescriptorSize : 0;
}

std::uintptr_t GfxContext::GetSrvCpuHandle(const std::uint32_t descriptorIndex) const
{
    if (m_impl == nullptr || !m_impl->SrvAllocator.IsValidIndex(descriptorIndex))
    {
        return 0;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_impl->SrvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(descriptorIndex) * m_impl->SrvDescriptorSize;
    return handle.ptr;
}

std::uint32_t GfxContext::AllocateOffscreenRtvBlock(const std::uint32_t count)
{
    if (m_impl == nullptr)
    {
        GfxContextDetail::SetGpuAllocationError("GfxContext is not initialized");
        return FixedDescriptorHeap::kInvalid;
    }

    const std::uint32_t index = m_impl->OffscreenRtvAllocator.AllocateBlock(count);
    if (index == FixedDescriptorHeap::kInvalid)
    {
        GfxContextDetail::SetGpuAllocationError("GfxContext out of offscreen RTV descriptors");
    }

    return index;
}

void GfxContext::FreeOffscreenRtvBlock(const std::uint32_t baseIndex, const std::uint32_t count)
{
    if (m_impl == nullptr)
    {
        return;
    }

    m_impl->OffscreenRtvAllocator.FreeBlock(baseIndex, count);
}

std::uint32_t GfxContext::AllocateOffscreenDsv()
{
    if (m_impl == nullptr)
    {
        GfxContextDetail::SetGpuAllocationError("GfxContext is not initialized");
        return FixedDescriptorHeap::kInvalid;
    }

    const std::uint32_t index = m_impl->OffscreenDsvAllocator.AllocateOne();
    if (index == FixedDescriptorHeap::kInvalid)
    {
        GfxContextDetail::SetGpuAllocationError("GfxContext out of offscreen DSV descriptors");
    }

    return index;
}

void GfxContext::FreeOffscreenDsv(const std::uint32_t descriptorIndex)
{
    if (m_impl == nullptr)
    {
        return;
    }

    m_impl->OffscreenDsvAllocator.FreeOne(descriptorIndex);
}

std::uintptr_t GfxContext::GetOffscreenRtvCpuHandle(const std::uint32_t descriptorIndex) const
{
    if (m_impl == nullptr || !m_impl->OffscreenRtvAllocator.IsValidIndex(descriptorIndex))
    {
        return 0;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_impl->OffscreenRtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(descriptorIndex) * m_impl->RtvDescriptorSize;
    return handle.ptr;
}

std::uintptr_t GfxContext::GetOffscreenDsvCpuHandle(const std::uint32_t descriptorIndex) const
{
    if (m_impl == nullptr || !m_impl->OffscreenDsvAllocator.IsValidIndex(descriptorIndex))
    {
        return 0;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_impl->OffscreenDsvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(descriptorIndex) * m_impl->DsvDescriptorSize;
    return handle.ptr;
}

void* GfxContext::GetSrvHeapGpuHandle(std::uint32_t descriptorIndex) const
{
    if (m_impl == nullptr || !m_impl->SrvAllocator.IsValidIndex(descriptorIndex))
    {
        return nullptr;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE handle = m_impl->SrvHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(descriptorIndex) * m_impl->SrvDescriptorSize;
    return reinterpret_cast<void*>(handle.ptr);
}

std::uint32_t GfxContext::AllocateOffscreenSrv()
{
    if (m_impl == nullptr)
    {
        GfxContextDetail::SetGpuAllocationError("GfxContext is not initialized");
        return FixedDescriptorHeap::kInvalid;
    }

    const std::uint32_t index = m_impl->SrvAllocator.AllocateOne();
    if (index == FixedDescriptorHeap::kInvalid)
    {
        GfxContextDetail::SetGpuAllocationError("GfxContext out of SRV descriptors");
    }

    return index;
}

void GfxContext::FreeOffscreenSrv(std::uint32_t descriptorIndex)
{
    if (m_impl == nullptr)
    {
        return;
    }

    m_impl->SrvAllocator.FreeOne(descriptorIndex);
}

void GfxContext::AllocSrvDescriptorForImGui(void* out_cpu_handle, void* out_gpu_handle)
{
    const std::uint32_t index = AllocateOffscreenSrv();
    auto* cpu = static_cast<D3D12_CPU_DESCRIPTOR_HANDLE*>(out_cpu_handle);
    auto* gpu = static_cast<D3D12_GPU_DESCRIPTOR_HANDLE*>(out_gpu_handle);
    if (index == FixedDescriptorHeap::kInvalid)
    {
        *cpu = {};
        *gpu = {};
        return;
    }

    *cpu = m_impl->SrvHeap->GetCPUDescriptorHandleForHeapStart();
    cpu->ptr += static_cast<SIZE_T>(index) * m_impl->SrvDescriptorSize;
    *gpu = m_impl->SrvHeap->GetGPUDescriptorHandleForHeapStart();
    gpu->ptr += static_cast<UINT64>(index) * m_impl->SrvDescriptorSize;
}

void GfxContext::FreeSrvDescriptorFromCpuHandle(const std::uintptr_t cpu_handle_ptr)
{
    const D3D12_CPU_DESCRIPTOR_HANDLE heapStart = m_impl->SrvHeap->GetCPUDescriptorHandleForHeapStart();
    const std::uint32_t index = static_cast<std::uint32_t>(
        (cpu_handle_ptr - heapStart.ptr) / m_impl->SrvDescriptorSize);
    FreeOffscreenSrv(index);
}

void GfxContext::CreateSrvForTexture(
    void* resource,
    int formatRgba_UNORM,
    std::uint32_t descriptorIndex,
    int width,
    int height,
    const std::uint32_t mipLevels) const
{
    auto* d3dResource = static_cast<ID3D12Resource*>(resource);
    auto* device = static_cast<ID3D12Device*>(GetDevice());
    auto* srvHeap = static_cast<ID3D12DescriptorHeap*>(GetSrvHeap());

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = srvHeap->GetCPUDescriptorHandleForHeapStart();
    cpuHandle.ptr += static_cast<SIZE_T>(descriptorIndex) * GetSrvDescriptorSize();

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = static_cast<DXGI_FORMAT>(formatRgba_UNORM);
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = mipLevels;
    device->CreateShaderResourceView(d3dResource, &srvDesc, cpuHandle);
    (void)width;
    (void)height;
}

void GfxContext::CreateMsaaDepthSrv(void* resource, const std::uint32_t descriptorIndex) const
{
    auto* d3dResource = static_cast<ID3D12Resource*>(resource);
    auto* device = static_cast<ID3D12Device*>(GetDevice());
    auto* srvHeap = static_cast<ID3D12DescriptorHeap*>(GetSrvHeap());

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = srvHeap->GetCPUDescriptorHandleForHeapStart();
    cpuHandle.ptr += static_cast<SIZE_T>(descriptorIndex) * GetSrvDescriptorSize();

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    device->CreateShaderResourceView(d3dResource, &srvDesc, cpuHandle);
}

void GfxContext::ClearOffscreenTarget(
    void* resource,
    const std::uint32_t rtvIndex,
    const float clearColor[4],
    const int width,
    const int height)
{
    if (m_impl == nullptr || resource == nullptr || rtvIndex == UINT32_MAX)
    {
        return;
    }

    auto* d3dResource = static_cast<ID3D12Resource*>(resource);
    auto* commandList = m_impl->CommandList.Get();

    D3D12_CPU_DESCRIPTOR_HANDLE offscreenRtv{};
    offscreenRtv.ptr = GetOffscreenRtvCpuHandle(rtvIndex);

    D3D12_RESOURCE_BARRIER toRenderTarget{};
    toRenderTarget.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRenderTarget.Transition.pResource = d3dResource;
    toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    toRenderTarget.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    commandList->ResourceBarrier(1, &toRenderTarget);

    commandList->OMSetRenderTargets(1, &offscreenRtv, FALSE, nullptr);

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor{0, 0, width, height};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    commandList->ClearRenderTargetView(offscreenRtv, clearColor, 0, nullptr);

    D3D12_RESOURCE_BARRIER toShaderResource{};
    toShaderResource.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toShaderResource.Transition.pResource = d3dResource;
    toShaderResource.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toShaderResource.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toShaderResource.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    commandList->ResourceBarrier(1, &toShaderResource);

    FrameContext& frame = m_impl->Frames[m_frameIndex];
    commandList->OMSetRenderTargets(1, &frame.RtvCpuHandle, FALSE, nullptr);
    D3D12_VIEWPORT swapViewport{};
    swapViewport.Width = static_cast<float>(m_width);
    swapViewport.Height = static_cast<float>(m_height);
    swapViewport.MaxDepth = 1.0f;
    D3D12_RECT swapScissor{0, 0, m_width, m_height};
    commandList->RSSetViewports(1, &swapViewport);
    commandList->RSSetScissorRects(1, &swapScissor);
}

void GfxContext::BindSwapChainRenderTarget(const bool clearColor)
{
    if (m_impl == nullptr)
    {
        return;
    }

    FrameContext& frame = m_impl->Frames[m_frameIndex];
    auto* commandList = m_impl->CommandList.Get();

    commandList->OMSetRenderTargets(1, &frame.RtvCpuHandle, FALSE, nullptr);

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(m_width);
    viewport.Height = static_cast<float>(m_height);
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor{0, 0, m_width, m_height};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);

    if (clearColor)
    {
        const float clear[] = {0.08f, 0.09f, 0.15f, 1.0f};
        commandList->ClearRenderTargetView(frame.RtvCpuHandle, clear, 0, nullptr);
    }

    m_boundOutputFramebuffer = nullptr;
}

void GfxContext::SetBoundOutputFramebuffer(const Framebuffer* framebuffer)
{
    m_boundOutputFramebuffer = framebuffer;
}

const Framebuffer* GfxContext::GetBoundOutputFramebuffer() const
{
    return m_boundOutputFramebuffer;
}

void GfxContext::SetMaterialTextureFilterMode(const TextureFilterMode mode)
{
    m_materialTextureFilterMode = mode;
}

TextureFilterMode GfxContext::GetMaterialTextureFilterMode() const
{
    return m_materialTextureFilterMode;
}

void GfxContext::SetMaterialTextureAnisotropy(const std::uint32_t anisotropy)
{
    m_materialTextureAnisotropy = std::clamp(anisotropy, 1u, 16u);
}

std::uint32_t GfxContext::GetMaterialTextureAnisotropy() const
{
    return m_materialTextureAnisotropy;
}

void GfxContext::SetMaterialTextureMipBias(const float mipBias)
{
    m_materialTextureMipBias = std::clamp(mipBias, -4.0f, 4.0f);
}

float GfxContext::GetMaterialTextureMipBias() const
{
    return m_materialTextureMipBias;
}

bool GfxContext::IsMsaaSampleCountSupported(const int sampleCount) const
{
    if (sampleCount <= 1)
    {
        return true;
    }

    if (sampleCount != 2 && sampleCount != 4 && sampleCount != 8)
    {
        return false;
    }

    return (m_supportedMsaaSampleCountsMask & static_cast<std::uint8_t>(1u << sampleCount)) != 0;
}

void GfxContext::SetActiveMsaaSampleCount(const int sampleCount)
{
    if (sampleCount <= 1)
    {
        m_activeMsaaSampleCount = 1;
        return;
    }

    if (!IsMsaaSampleCountSupported(sampleCount))
    {
        return;
    }

    m_activeMsaaSampleCount = sampleCount;
}

bool GfxContext::IsRaytracingSupported() const
{
    return m_raytracingTier >= static_cast<int>(D3D12_RAYTRACING_TIER_1_0);
}

void GfxContext::GetOutputRenderSize(int& outWidth, int& outHeight) const
{
    outWidth = m_width;
    outHeight = m_height;

    if (m_boundOutputFramebuffer != nullptr)
    {
        const int framebufferWidth = m_boundOutputFramebuffer->GetWidth();
        const int framebufferHeight = m_boundOutputFramebuffer->GetHeight();
        if (framebufferWidth > 0 && framebufferHeight > 0)
        {
            outWidth = framebufferWidth;
            outHeight = framebufferHeight;
        }
    }
}

void GfxContext::ExecuteImmediate(const std::function<void(void* commandList)>& recordCommands)
{
    if (m_impl == nullptr || !recordCommands)
    {
        return;
    }

    if (m_impl->ImmediateUploadDepth > 0)
    {
        throw std::runtime_error(
            "Nested GPU upload (ExecuteImmediate called while another upload is in progress)");
    }

    if (m_frameRecording)
    {
        throw std::runtime_error(
            "ExecuteImmediate called during active frame recording; upload on the current command list instead");
    }

    struct ImmediateUploadScope
    {
        Impl* impl;
        explicit ImmediateUploadScope(Impl* implIn) : impl(implIn) { ++impl->ImmediateUploadDepth; }
        ~ImmediateUploadScope() { --impl->ImmediateUploadDepth; }
    };
    ImmediateUploadScope uploadScope(m_impl);

    ComPtr<ID3D12CommandAllocator> uploadAllocator;
    ComPtr<ID3D12GraphicsCommandList> uploadCommandList;
    ThrowIfFailed(
        m_impl->Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&uploadAllocator)),
        "Create upload command allocator failed");
    ThrowIfFailed(
        m_impl->Device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            uploadAllocator.Get(),
            nullptr,
            IID_PPV_ARGS(&uploadCommandList)),
        "Create upload command list failed");

    recordCommands(uploadCommandList.Get());
    ThrowIfFailed(uploadCommandList->Close(), "Close upload command list failed");

    ID3D12CommandList* commandLists[] = {uploadCommandList.Get()};
    m_impl->CommandQueue->ExecuteCommandLists(1, commandLists);

    std::uint64_t maxFenceValue = m_impl->Fence->GetCompletedValue();
    maxFenceValue = std::max(maxFenceValue, m_submissionFenceValue);
    for (const FrameContext& frameContext : m_impl->Frames)
    {
        maxFenceValue = std::max(maxFenceValue, frameContext.FenceValue);
    }

    const std::uint64_t fenceValue = AllocateNextFenceValue(maxFenceValue);
    ThrowIfFailed(m_impl->CommandQueue->Signal(m_impl->Fence.Get(), fenceValue), "Upload signal failed");
    if (FrameDiagnostics::IsEnabled())
    {
        FrameDiagnostics::Log(
            m_frameRecording
                ? "ExecuteImmediate: upload during active frame recording"
                : "ExecuteImmediate: upload outside frame recording");
    }
    WaitForFenceValue(fenceValue);
    m_submissionFenceValue = fenceValue;
}

void GfxContext::ResetDrawSrvTable()
{
    if (m_impl != nullptr)
    {
        m_impl->DrawSrvTableNextIndex = DrawTextureDescriptorStart;
    }
}

std::uint32_t GfxContext::AllocateDrawSrvTable()
{
    if (m_impl == nullptr)
    {
        GfxContextDetail::SetGpuAllocationError("GfxContext is not initialized");
        return UINT32_MAX;
    }

    const std::uint32_t tableStart = m_impl->DrawSrvTableNextIndex;
    const std::uint32_t nextIndex = tableStart + DrawTextureSlotsPerTable;
    if (nextIndex > OffscreenSrvDescriptorStart)
    {
        GfxContextDetail::SetGpuAllocationError("Draw SRV descriptor table exhausted");
        return UINT32_MAX;
    }

    m_impl->DrawSrvTableNextIndex = nextIndex;
    return tableStart;
}

GfxContext::TransientUploadAllocation GfxContext::AllocateTransientUpload(
    const void* data,
    const std::uint32_t byteSize)
{
    TransientUploadAllocation allocation{};
    if (m_impl == nullptr || data == nullptr || byteSize == 0)
    {
        return allocation;
    }

    TransientUploadArena& arena = m_impl->TransientUploadArenas[m_frameIndex];
    if (arena.Resource == nullptr || arena.Mapped == nullptr)
    {
        EngineDiagnostics::SetLastGpuAllocationError("Transient upload arena is not initialized");
        return allocation;
    }

    const std::uint32_t alignedSize = (byteSize + 255u) & ~255u;
    if (arena.Offset + alignedSize > TransientUploadCapacityBytes)
    {
        EngineDiagnostics::SetLastGpuAllocationError("Transient upload arena exhausted");
        return allocation;
    }

    std::memcpy(static_cast<std::uint8_t*>(arena.Mapped) + arena.Offset, data, byteSize);
    allocation.gpuAddress =
        arena.Resource->GetGPUVirtualAddress() + static_cast<std::uint64_t>(arena.Offset);
    allocation.byteSize = byteSize;
    arena.Offset += alignedSize;
    return allocation;
}

void GfxContext::WaitForFenceValue(const std::uint64_t fenceValue)
{
    if (m_impl == nullptr || fenceValue == 0)
    {
        return;
    }

    const auto waitStart = std::chrono::steady_clock::now();
    long long lastLoggedMs = -1;

    while (m_impl->Fence->GetCompletedValue() < fenceValue)
    {
        PumpWindowEvents(m_window);

        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - waitStart)
                                   .count();
        if (FrameDiagnostics::IsEnabled() && elapsedMs >= 500 && elapsedMs / 500 != lastLoggedMs / 500)
        {
            lastLoggedMs = elapsedMs;
            FrameDiagnostics::LogFenceWait(
                fenceValue,
                m_impl->Fence->GetCompletedValue(),
                elapsedMs);
        }

        std::string deviceRemovedReason;
        if (IsDeviceRemoved(&deviceRemovedReason))
        {
            throw std::runtime_error("D3D12 device removed while waiting for fence: " + deviceRemovedReason);
        }

        const HRESULT setEventResult =
            m_impl->Fence->SetEventOnCompletion(fenceValue, m_impl->FenceEvent);
        if (FAILED(setEventResult))
        {
            if (IsDeviceRemoved(&deviceRemovedReason))
            {
                throw std::runtime_error(
                    "D3D12 device removed while waiting for fence: " + deviceRemovedReason);
            }

            ThrowIfFailed(setEventResult, "SetEventOnCompletion failed");
        }

        const DWORD waitResult = WaitForSingleObject(m_impl->FenceEvent, 16);
        if (waitResult == WAIT_OBJECT_0)
        {
            break;
        }

        if (m_window != nullptr && glfwWindowShouldClose(m_window))
        {
            break;
        }
    }
}

bool GfxContext::IsDeviceRemoved(std::string* outReason) const
{
    if (m_impl == nullptr || m_impl->Device == nullptr)
    {
        return false;
    }

    const HRESULT removedReason = m_impl->Device->GetDeviceRemovedReason();
    if (removedReason == S_OK)
    {
        return false;
    }

    if (outReason != nullptr)
    {
        *outReason = "HRESULT=" + GfxContextDetail::FormatHresult(removedReason);
    }

    return true;
}

void GfxContext::WaitForGpuIdle()
{
    WaitForGpu();
}

void GfxContext::WaitForSwapchainFrames()
{
    if (m_impl == nullptr)
    {
        return;
    }

    std::uint64_t maxFenceValue = m_submissionFenceValue;
    for (const FrameContext& frame : m_impl->Frames)
    {
        maxFenceValue = std::max(maxFenceValue, frame.FenceValue);
    }

    FrameDiagnostics::LogPhase("WaitForSwapchainFrames");
    WaitForFenceValue(maxFenceValue);
}

void GfxContext::WaitForGpu()
{
    if (m_impl == nullptr)
    {
        return;
    }

    std::uint64_t maxFenceValue = 0;
    for (const FrameContext& frame : m_impl->Frames)
    {
        maxFenceValue = std::max(maxFenceValue, frame.FenceValue);
    }

    maxFenceValue = std::max(maxFenceValue, m_submissionFenceValue);

    for (const std::uint64_t fenceValue : m_fenceValues)
    {
        maxFenceValue = std::max(maxFenceValue, fenceValue);
    }

    WaitForFenceValue(maxFenceValue);
}

void GfxContext::SignalFrameSubmission()
{
    FrameContext& frame = m_impl->Frames[m_frameIndex];
    const std::uint64_t nextFenceValue = AllocateNextFenceValue(frame.FenceValue);
    ThrowIfFailed(m_impl->CommandQueue->Signal(m_impl->Fence.Get(), nextFenceValue), "Signal failed");
    frame.FenceValue = nextFenceValue;
    m_fenceValues[m_frameIndex] = nextFenceValue;
    m_submissionFenceValue = std::max(m_submissionFenceValue, nextFenceValue);
}

void GfxContext::AdvanceSwapchainFrameIndex()
{
    m_frameIndex = m_impl->SwapChain->GetCurrentBackBufferIndex();
}

void GfxContext::MoveToNextFrame()
{
    SignalFrameSubmission();
    AdvanceSwapchainFrameIndex();
}

std::uint64_t GfxContext::AllocateNextFenceValue(const std::uint64_t frameFenceValue) const
{
    std::uint64_t nextFenceValue = frameFenceValue + 1;
    nextFenceValue = std::max(nextFenceValue, m_submissionFenceValue + 1);
    if (m_impl != nullptr && m_impl->Fence != nullptr)
    {
        nextFenceValue = std::max(nextFenceValue, m_impl->Fence->GetCompletedValue() + 1);
    }

    return nextFenceValue;
}

void GfxContext::CreateRenderTargets()
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_impl->RtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (std::uint32_t frameIndex = 0; frameIndex < FrameCount; ++frameIndex)
    {
        FrameContext& frame = m_impl->Frames[frameIndex];
        ThrowIfFailed(m_impl->SwapChain->GetBuffer(frameIndex, IID_PPV_ARGS(&frame.RenderTarget)), "GetBuffer failed");
        m_impl->Device->CreateRenderTargetView(frame.RenderTarget.Get(), nullptr, rtvHandle);
        frame.RtvCpuHandle = rtvHandle;
        rtvHandle.ptr += m_impl->RtvDescriptorSize;
    }

    m_frameIndex = m_impl->SwapChain->GetCurrentBackBufferIndex();
}

void GfxContext::ReleaseRenderTargets()
{
    for (FrameContext& frame : m_impl->Frames)
    {
        frame.RenderTarget.Reset();
    }
}

void GfxContext::RenderImGui(ImDrawData* drawData)
{
    ImGui_ImplDX12_RenderDrawData(drawData, m_impl->CommandList.Get());
}

bool GfxContext::ReadbackPresentedColorPixel(const int x, const int y, float outRgba[4]) const
{
    if (m_impl == nullptr || outRgba == nullptr || m_width <= 0 || m_height <= 0)
    {
        return false;
    }

    const std::uint32_t presentedIndex = (m_frameIndex + FrameCount - 1u) % FrameCount;
    ID3D12Resource* swapchainBuffer = m_impl->Frames[presentedIndex].RenderTarget.Get();
    if (swapchainBuffer == nullptr)
    {
        return false;
    }

    D3D12MA::Allocator* allocator = m_impl->MemoryAllocator;
    constexpr UINT64 kReadbackSize = sizeof(std::uint8_t) * 4;

    D3D12_RESOURCE_DESC readbackDesc{};
    readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readbackDesc.Width = kReadbackSize;
    readbackDesc.Height = 1;
    readbackDesc.DepthOrArraySize = 1;
    readbackDesc.MipLevels = 1;
    readbackDesc.Format = DXGI_FORMAT_UNKNOWN;
    readbackDesc.SampleDesc.Count = 1;
    readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12MA::ALLOCATION_DESC readbackAllocationDesc{};
    readbackAllocationDesc.HeapType = D3D12_HEAP_TYPE_READBACK;

    ID3D12Resource* readbackResource = nullptr;
    D3D12MA::Allocation* readbackAllocation = nullptr;
    if (FAILED(allocator->CreateResource(
            &readbackAllocationDesc,
            &readbackDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            &readbackAllocation,
            IID_PPV_ARGS(&readbackResource))))
    {
        return false;
    }

    const int clampedX = std::clamp(x, 0, m_width - 1);
    const int clampedY = std::clamp(y, 0, m_height - 1);

    const_cast<GfxContext*>(this)->ExecuteImmediate([&](void* commandListPtr) {
        auto* commandList = static_cast<ID3D12GraphicsCommandList*>(commandListPtr);

        D3D12_RESOURCE_BARRIER toCopy{};
        toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopy.Transition.pResource = swapchainBuffer;
        toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        commandList->ResourceBarrier(1, &toCopy);

        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = swapchainBuffer;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = readbackResource;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint.Offset = 0;
        destination.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        destination.PlacedFootprint.Footprint.Width = 1;
        destination.PlacedFootprint.Footprint.Height = 1;
        destination.PlacedFootprint.Footprint.Depth = 1;
        destination.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(kReadbackSize);

        const UINT left = static_cast<UINT>(clampedX);
        const UINT top = static_cast<UINT>(clampedY);
        const D3D12_BOX sourceBox{left, top, 0, left + 1, top + 1, 1};
        commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, &sourceBox);

        D3D12_RESOURCE_BARRIER toPresent{};
        toPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toPresent.Transition.pResource = swapchainBuffer;
        toPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        commandList->ResourceBarrier(1, &toPresent);
    });

    D3D12_RANGE readRange{0, static_cast<SIZE_T>(kReadbackSize)};
    void* mapped = nullptr;
    if (FAILED(readbackResource->Map(0, &readRange, &mapped)))
    {
        readbackAllocation->Release();
        readbackResource->Release();
        return false;
    }

    const auto* bytes = static_cast<const std::uint8_t*>(mapped);
    for (int channel = 0; channel < 4; ++channel)
    {
        outRgba[channel] = static_cast<float>(bytes[channel]) / 255.0f;
    }

    readbackResource->Unmap(0, nullptr);
    readbackAllocation->Release();
    readbackResource->Release();
    return true;
}

std::string GfxContext::GetLastGpuAllocationError()
{
    return EngineDiagnostics::GetLastGpuAllocationError();
}

void GfxContext::GetSrvDescriptorUsage(std::uint32_t& outUsed, std::uint32_t& outCapacity) const
{
    outUsed = 0;
    outCapacity = 0;
    if (m_impl == nullptr)
    {
        return;
    }

    outUsed = m_impl->SrvAllocator.UsedCount();
    outCapacity = m_impl->SrvAllocator.Capacity();
}
