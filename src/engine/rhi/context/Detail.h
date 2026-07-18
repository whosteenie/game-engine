#pragma once

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
    void SetGpuAllocationError(const char* message);
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

