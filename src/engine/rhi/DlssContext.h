#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

struct ID3D12Device;
struct IDXGIAdapter;

// NVIDIA DLSS via Streamline (devdoc/dlss-super-resolution.md).
//
// Phase S0 scope: dynamically load sl.interposer.dll, initialize Streamline, bind our D3D12 device,
// and probe DLSS support for the active adapter. Nothing here touches the render pipeline yet —
// later phases add resource tagging, constants, and slEvaluateFeature.
//
// We deliberately load the interposer at runtime (rather than static-linking sl.interposer.lib) so
// our existing dxgi.lib/d3d12.lib linkage and the entire non-DLSS pipeline are untouched. As a
// consequence SL never interposes DXGI/D3D (we drive DLSS manually), which also makes it safe to run
// the multi-second NGX cold-init OFF the main thread: BeginAsyncInitialize kicks the whole init +
// support probe onto a worker so app startup isn't blocked. DLSS consumers must gate on IsReady()
// (and IsDlssSupported()). Compiled to a stub when GAME_ENGINE_ENABLE_DLSS is not defined.
class DlssContext
{
public:
    static DlssContext& Get();

    // Launch Streamline init + DLSS support probe on a background thread. Call once, right after the
    // D3D12 device + adapter are created. Returns immediately; poll IsReady()/IsDlssSupported().
    void BeginAsyncInitialize(ID3D12Device* device, IDXGIAdapter* adapter);

    // Call before the D3D12 device is destroyed. Joins the init worker, then shuts SL down.
    void Shutdown();

    // Init worker has finished (successfully or not). false while still probing at startup.
    bool IsReady() const { return m_ready.load(std::memory_order_acquire); }
    bool IsRuntimeInitialized() const { return m_initialized.load(std::memory_order_acquire); }
    bool IsDlssSupported() const { return m_supported.load(std::memory_order_acquire); }

    // Thread-safe snapshot of the human-readable status (worker updates it as it progresses).
    std::string StatusString() const;

private:
    DlssContext() = default;
    ~DlssContext() = default;
    DlssContext(const DlssContext&) = delete;
    DlssContext& operator=(const DlssContext&) = delete;

    void RunInitialize(ID3D12Device* device, IDXGIAdapter* adapter);
    void SetStatus(std::string status);

    std::thread m_worker;
    std::atomic<bool> m_started{false};
    std::atomic<bool> m_ready{false};       // worker finished
    std::atomic<bool> m_initialized{false}; // slInit succeeded + function pointers resolved
    std::atomic<bool> m_supported{false};   // slIsFeatureSupported(DLSS) == eOk for this adapter

    mutable std::mutex m_statusMutex;
    std::string m_status = "DLSS: initializing…";
    void* m_interposer = nullptr; // HMODULE for sl.interposer.dll (dynamic load)
};
