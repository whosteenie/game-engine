#include "engine/rhi/GfxContext.h"

#include "engine/rhi/HresultFormat.h"

#include "engine/platform/system/CrashHandler.h"
#include "engine/platform/diagnostics/FrameDiagnostics.h"
#include "engine/platform/diagnostics/EngineDiagnostics.h"
#include "engine/platform/diagnostics/EngineLog.h"
#include "engine/rhi/DlssContext.h"
#include "engine/rendering/core/DxrCapabilities.h"
#include "engine/rendering/resources/Framebuffer.h"
#include "engine/rhi/d3d12/D3D12Throw.h"
#include "engine/rhi/d3d12/FixedDescriptorHeap.h"

#include <imgui.h>
#include <imgui_impl_dx12.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <D3D12MemAlloc.h>
#include <d3d10.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <winternl.h>

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

    void ConfigureD3D12InfoQueueBreaks(ID3D12Device* device)
    {
#if defined(_DEBUG) && defined(GAME_ENGINE_D3D12_DEBUG_LAYER)
        if (device == nullptr || !IsDebuggerPresent())
        {
            return;
        }

        ComPtr<ID3D12InfoQueue> infoQueue;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&infoQueue))) || infoQueue == nullptr)
        {
            return;
        }

        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
        EngineLog::Info("gfx", "D3D12 debug-layer breaks enabled for corruption/error messages.");
#else
        (void)device;
#endif
    }

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
    ComPtr<ID3D12DescriptorHeap> SrvCpuHeap;
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

    // S0-P5: a capture is copied by EndFrame on the normal command list and retired only after
    // that frame's fence. It deliberately has no independent queue submission.
    struct PresentedImageCaptureState
    {
        ID3D12Resource* Resource = nullptr;
        D3D12MA::Allocation* Allocation = nullptr;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint{};
        UINT64 FenceValue = 0;
        int Width = 0;
        int Height = 0;
        bool Requested = false;
        bool Submitted = false;
    } PresentedImageCapture;

    std::uint32_t DrawSrvTableNextIndex = GfxContext::DrawTextureDescriptorStart;

    // CRASH-01/CRASH-03: releases queued until the covering fence value has completed.
    struct DeferredDestroy
    {
        std::uint64_t FenceValue = 0;
        D3D12MA::Allocation* Allocation = nullptr;
        ID3D12Resource* Resource = nullptr;
        IUnknown* GpuObject = nullptr;
        std::uint32_t SrvIndex = UINT32_MAX;
        std::uint32_t RtvBaseIndex = UINT32_MAX;
        std::uint32_t RtvCount = 0;
        std::uint32_t DsvIndex = UINT32_MAX;
    };
    std::vector<DeferredDestroy> DeferredDestroys;
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

    const auto pumpEvents = [this]() { PumpWindowEvents(m_window); };

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
    pumpEvents();

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
    pumpEvents();

    ConfigureD3D12InfoQueueBreaks(m_impl->Device.Get());

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
        m_adapterDedicatedVideoMemory = adapterDesc.DedicatedVideoMemory;
        m_dxrRuntimeSnapshot = {};
        m_dxrRuntimeSnapshot.adapterDescription = m_adapterDescription;
        m_dxrRuntimeSnapshot.adapterVendorId = adapterDesc.VendorId;
        m_dxrRuntimeSnapshot.adapterDeviceId = adapterDesc.DeviceId;

        LARGE_INTEGER driverVersion{};
        if (SUCCEEDED(m_impl->Adapter->CheckInterfaceSupport(__uuidof(ID3D10Device), &driverVersion)))
        {
            const std::uint64_t version = static_cast<std::uint64_t>(driverVersion.QuadPart);
            m_dxrRuntimeSnapshot.driverVersion = std::to_string((version >> 48u) & 0xffffu) + "."
                + std::to_string((version >> 32u) & 0xffffu) + "."
                + std::to_string((version >> 16u) & 0xffffu) + "."
                + std::to_string(version & 0xffffu);
        }

        RTL_OSVERSIONINFOW osVersion{};
        osVersion.dwOSVersionInfoSize = sizeof(osVersion);
        using RtlGetVersionFn = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
        const auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        if (rtlGetVersion != nullptr && rtlGetVersion(&osVersion) == 0)
        {
            m_dxrRuntimeSnapshot.osVersion = std::to_string(osVersion.dwMajorVersion) + "."
                + std::to_string(osVersion.dwMinorVersion) + "." + std::to_string(osVersion.dwBuildNumber);
        }
#if defined(GAME_ENGINE_AGILITY_SDK_PACKAGE_VERSION)
        m_dxrRuntimeSnapshot.agilityPackageVersion = GAME_ENGINE_AGILITY_SDK_PACKAGE_VERSION;
#endif
#if defined(GAME_ENGINE_AGILITY_SDK_VERSION)
        m_dxrRuntimeSnapshot.agilityLoaderVersion = std::to_string(GAME_ENGINE_AGILITY_SDK_VERSION);
#endif

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

        D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{};
        shaderModel.HighestShaderModel = D3D_HIGHEST_SHADER_MODEL;
        if (SUCCEEDED(m_impl->Device->CheckFeatureSupport(
                D3D12_FEATURE_SHADER_MODEL,
                &shaderModel,
                sizeof(shaderModel))))
        {
            m_highestShaderModel = static_cast<int>(shaderModel.HighestShaderModel);
        }
        else
        {
            m_highestShaderModel = 0;
        }

#if D3D12_SDK_VERSION >= 618
        D3D12_FEATURE_DATA_D3D12_OPTIONS22 options22{};
        if (SUCCEEDED(m_impl->Device->CheckFeatureSupport(
                D3D12_FEATURE_D3D12_OPTIONS22,
                &options22,
                sizeof(options22))))
        {
            m_dxrRuntimeSnapshot.options22Query = "succeeded";
            m_dxrRuntimeSnapshot.options22ActuallyReorders =
                options22.ShaderExecutionReorderingActuallyReorders != FALSE;
            m_dxrRuntimeSnapshot.options22ByteOffsetViewsSupported =
                options22.CreateByteOffsetViewsSupported != FALSE;
            m_dxrRuntimeSnapshot.options22Max1DDispatchSize = options22.Max1DDispatchSize;
            m_dxrRuntimeSnapshot.options22Max1DDispatchMeshSize = options22.Max1DDispatchMeshSize;
        }
        else
        {
            m_dxrRuntimeSnapshot.options22Query = "unsupported_or_query_failed";
        }
#else
        m_dxrRuntimeSnapshot.options22Query = "unavailable_in_build_sdk";
#endif

        // Some drivers report tier 0 on ID3D12Device even when ID3D12Device5 + AS builds work.
        if (m_raytracingTier < static_cast<int>(D3D12_RAYTRACING_TIER_1_0))
        {
            Microsoft::WRL::ComPtr<ID3D12Device5> device5;
            if (SUCCEEDED(m_impl->Device->QueryInterface(IID_PPV_ARGS(&device5))) && device5 != nullptr)
            {
                D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5OnDevice5{};
                if (SUCCEEDED(device5->CheckFeatureSupport(
                        D3D12_FEATURE_D3D12_OPTIONS5,
                        &options5OnDevice5,
                        sizeof(options5OnDevice5))))
                {
                    m_raytracingTier = static_cast<int>(options5OnDevice5.RaytracingTier);
                }

                if (m_raytracingTier < static_cast<int>(D3D12_RAYTRACING_TIER_1_0))
                {
                    m_raytracingTier = static_cast<int>(D3D12_RAYTRACING_TIER_1_0);
                    EngineLog::Warn(
                        "gfx",
                        "D3D12_OPTIONS5 reported no ray tracing tier; ID3D12Device5 is available — "
                        "assuming Tier 1.0+.");
                }
            }
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
        if (SUCCEEDED(m_impl->Device->CheckFeatureSupport(
                D3D12_FEATURE_D3D12_OPTIONS7,
                &options7,
                sizeof(options7))))
        {
            m_meshShaderTier = static_cast<int>(options7.MeshShaderTier);
        }
        else
        {
            m_meshShaderTier = 0;
        }

        EngineLog::Info(
            "gfx",
            "Adapter: " + m_adapterDescription + " | Ray tracing tier: "
                + GetRaytracingTierLabel(m_raytracingTier) + " | Shader model: "
                + GetShaderModelLabel(m_highestShaderModel) + " | Inline ray tracing: "
                + (IsInlineRaytracingSupported() ? "yes" : "no") + " | SER: "
                + (IsShaderExecutionReorderingSupported() ? "yes" : "no") + " | DXR library: "
                + GetPreferredDxrLibraryProfile() + " | Mesh shader tier: " + std::to_string(m_meshShaderTier));
        if (m_raytracingTier == 0)
        {
            EngineLog::Warn(
                "gfx",
                "Ray tracing is not supported on this GPU or driver. DXR features will be disabled.");
        }
        if (m_meshShaderTier == 0)
        {
            EngineLog::Warn(
                "gfx",
                "Mesh shaders are not supported on this GPU or driver. Mesh-shader scene rendering will be disabled.");
        }

        m_dxrRuntimeSnapshot.raytracingTier = m_raytracingTier;
        m_dxrRuntimeSnapshot.highestShaderModel = m_highestShaderModel;
        EngineLog::Info("dxr-runtime", SerializeDxrRuntimeSnapshotJson(m_dxrRuntimeSnapshot));

        // DLSS/Streamline (S0): init SL + probe DLSS support on a background thread so the
        // multi-second NGX cold-init doesn't block editor startup. SL doesn't interpose our
        // DXGI/D3D (manual DLSS eval), so off-thread init is safe; consumers gate on IsReady().
        DlssContext::Get().BeginAsyncInitialize(m_impl->Device.Get(), m_impl->Adapter.Get());

        // On a hard crash, dump the D3D12 debug-layer info queue so the last validation message
        // (the actual reason behind driver/debug-layer faults) is logged right before the stack.
        CrashHandler::SetContextHook([]() { GfxContext::Get().LogD3D12InfoQueueMessages("crash"); });
    }
    pumpEvents();

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
    pumpEvents();

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(
        m_impl->Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_impl->CommandQueue)),
        "CreateCommandQueue failed");

    ThrowIfFailed(
        m_impl->Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_impl->ImGuiUploadQueue)),
        "Create ImGui upload command queue failed");

    UINT64 timestampFrequency = 0;
    if (SUCCEEDED(m_impl->CommandQueue->GetTimestampFrequency(&timestampFrequency)))
    {
        m_gpuProfiler.Initialize(m_impl->Device.Get(), FrameCount, timestampFrequency);
    }

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
    pumpEvents();

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

    D3D12_DESCRIPTOR_HEAP_DESC srvCpuHeapDesc = srvHeapDesc;
    srvCpuHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(
        m_impl->Device->CreateDescriptorHeap(&srvCpuHeapDesc, IID_PPV_ARGS(&m_impl->SrvCpuHeap)),
        "Create CPU-readable SRV heap failed");

    m_impl->SrvDescriptorSize = m_impl->Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_impl->SrvAllocator = FixedDescriptorHeap(OffscreenSrvDescriptorCount);
    m_impl->SrvAllocator.SetIndexOffset(OffscreenSrvDescriptorStart);
    pumpEvents();

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
    pumpEvents();

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
    pumpEvents();

    return true;
}

void GfxContext::WaitForImGuiUploadQueueIdle()
{
    if (m_impl == nullptr || m_impl->ImGuiUploadQueue == nullptr || m_impl->Fence == nullptr)
    {
        return;
    }

    const std::uint64_t fenceValue = AllocateNextFenceValue(m_submissionFenceValue);
    if (FAILED(m_impl->ImGuiUploadQueue->Signal(m_impl->Fence.Get(), fenceValue)))
    {
        return;
    }

    m_submissionFenceValue = std::max(m_submissionFenceValue, fenceValue);
    WaitForFenceValue(fenceValue);
}

void GfxContext::PrepareForDeviceShutdown()
{
    if (m_impl == nullptr)
    {
        return;
    }

    // Closing the window can leave the final frame recording. WaitForGpu cannot cover an
    // unsubmitted command list, and ResetCommandListForTeardown deliberately refuses to reset one;
    // releasing a DXR state object afterward therefore leaves the recorded RTPSO referenced.
    // Finish/cancel that frame first so its submission receives a fence and can be drained below.
    if (m_frameRecording)
    {
        CancelFrame();
    }

    WaitForGpu();
    WaitForImGuiUploadQueueIdle();
    ResetCommandListForTeardown();
    ProcessDeferredDestroys(true);
}

void GfxContext::Shutdown()
{
    if (m_impl == nullptr)
    {
        return;
    }

    PrepareForDeviceShutdown();
    // PrepareForDeviceShutdown drained the fence governing this capture; it can now release its
    // readback allocation without creating another queue submission during shutdown.
    ReleasePresentedImageCapture();
    // Drop swapchain back-buffer refs before Streamline teardown (upgraded swapchains must have
    // zero outstanding references when slShutdown runs).
    ReleaseRenderTargets();

    const bool swapChainOwnedByStreamline = DlssContext::Get().IsSwapChainUpgraded();
    // DLSS/Streamline (S0): shut SL down while the device is still alive.
    DlssContext::Get().Shutdown();
    // slShutdown / NGX teardown can enqueue device work; wait again before releasing heaps.
    WaitForGpu();
    WaitForImGuiUploadQueueIdle();
    ResetCommandListForTeardown();
    ProcessDeferredDestroys(true);
    // slUpgradeInterface hands ownership of the proxy swapchain to Streamline; slShutdown destroys
    // it. Detach so ~ComPtr does not Release() an already-freed object (shutdown access violation).
    if (swapChainOwnedByStreamline)
    {
        m_impl->SwapChain.Detach();
    }
    m_gpuProfiler.Shutdown();

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

void GfxContext::RebindFrameDescriptorHeaps()
{
    if (m_impl == nullptr || !m_frameRecording || m_impl->CommandList == nullptr)
    {
        return;
    }

    ID3D12DescriptorHeap* heaps[] = {m_impl->SrvHeap.Get()};
    m_impl->CommandList->SetDescriptorHeaps(1, heaps);
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

bool GfxContext::IsInlineRaytracingSupported() const
{
    return DxrFeatureCapabilities{m_raytracingTier, m_highestShaderModel}.SupportsInlineRaytracing();
}

bool GfxContext::IsShaderExecutionReorderingSupported() const
{
    return DxrFeatureCapabilities{m_raytracingTier, m_highestShaderModel}
        .SupportsShaderExecutionReordering();
}

bool GfxContext::SupportsModernDxrLibrary() const
{
    return DxrFeatureCapabilities{m_raytracingTier, m_highestShaderModel}.SupportsModernDxrLibrary();
}

const char* GfxContext::GetPreferredDxrLibraryProfile() const
{
    return DxrFeatureCapabilities{m_raytracingTier, m_highestShaderModel}.GetPreferredLibraryProfile();
}

void GfxContext::SetDxrRuntimeSerPolicy(const char* const policy)
{
    m_dxrRuntimeSnapshot.requestedSerPolicy = policy != nullptr ? policy : "missing";
    EngineLog::Info("dxr-runtime", SerializeDxrRuntimeSnapshotJson(m_dxrRuntimeSnapshot));
}

void GfxContext::ReportDxrPathTracerPipelineResult(
    const bool diagnosticPermutation,
    const bool serPermutation,
    const char* const compilerLibraryResult,
    const char* const rtpsoResult,
    const char* const fallbackReason)
{
    const std::size_t permutationIndex = serPermutation
        ? (diagnosticPermutation ? DxrRuntimeSerDiagnostic : DxrRuntimeSerProduction)
        : (diagnosticPermutation ? DxrRuntimeFallbackDiagnostic : DxrRuntimeFallbackProduction);
    DxrRuntimeSnapshot::PermutationResult& result = m_dxrRuntimeSnapshot.permutations[permutationIndex];
    result.compilerLibrary = compilerLibraryResult != nullptr ? compilerLibraryResult : "missing";
    result.rtpso = rtpsoResult != nullptr ? rtpsoResult : "missing";
    if (fallbackReason != nullptr && fallbackReason[0] != '\0')
    {
        m_dxrRuntimeSnapshot.fallbackReason = fallbackReason;
    }
    EngineLog::Info("dxr-runtime", SerializeDxrRuntimeSnapshotJson(m_dxrRuntimeSnapshot));
}

void GfxContext::ReportDxrPathTracerSelection(
    const bool diagnosticPermutation,
    const bool serPermutation,
    const char* const fallbackReason)
{
    m_dxrRuntimeSnapshot.selectedPermutation = serPermutation
        ? (diagnosticPermutation ? "ser_diagnostic" : "ser_production")
        : (diagnosticPermutation ? "fallback_diagnostic" : "fallback_production");
    m_dxrRuntimeSnapshot.dispatchedPermutation = "not_dispatched";
    m_dxrRuntimeSnapshot.fallbackReason = fallbackReason != nullptr ? fallbackReason : "missing";
    EngineLog::Info("dxr-runtime", SerializeDxrRuntimeSnapshotJson(m_dxrRuntimeSnapshot));
}

void GfxContext::ReportDxrPathTracerDispatch(
    const bool diagnosticPermutation,
    const bool serPermutation)
{
    m_dxrRuntimeSnapshot.dispatchedPermutation = serPermutation
        ? (diagnosticPermutation ? "ser_diagnostic" : "ser_production")
        : (diagnosticPermutation ? "fallback_diagnostic" : "fallback_production");
    EngineLog::Info("dxr-runtime", SerializeDxrRuntimeSnapshotJson(m_dxrRuntimeSnapshot));
}

std::uint64_t GfxContext::GetCompletedFenceValue() const
{
    return m_impl != nullptr && m_impl->Fence != nullptr ? m_impl->Fence->GetCompletedValue() : 0;
}

std::uint64_t GfxContext::GetPendingFrameFenceValue() const
{
    if (m_impl == nullptr)
    {
        return m_submissionFenceValue;
    }

    if (!m_frameRecording)
    {
        return m_submissionFenceValue;
    }

    return AllocateNextFenceValue(m_impl->Frames[m_frameIndex].FenceValue);
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

