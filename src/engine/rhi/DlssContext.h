#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

struct ID3D12Device;
struct IDXGIAdapter;

// DLSS quality selector. Mirrors the editor's DLAA AntiAliasingMode + DlssPreset combination but is
// kept engine-agnostic so this header carries no <sl.h>/<d3d12.h> dependency (it must also compile in
// the GAME_ENGINE_ENABLE_DLSS=off stub build).
enum class DlssQuality
{
    DLAA,
    Quality,
    Balanced,
    Performance,
    UltraPerformance,
};

// Plain-old-data inputs for a single DLSS evaluate. Native pointers are ID3D12Resource*/command list;
// states are D3D12_RESOURCE_STATES (as uint). Matrices are 16 floats each; the engine passes glm
// column-major matrices which map byte-for-byte onto Streamline's row-major/row-vector float4x4 (see
// DlssContext.cpp for the derivation). Jitter is in pixels; mvecScale normalizes the NDC motion-vector
// buffer into Streamline's expected [-1,1] range (see the reflections/velocity convention notes).
struct DlssFrameInputs
{
    void* commandList = nullptr;

    void* colorInput = nullptr; // scaling input color (render res)
    unsigned int colorInputState = 0;
    void* colorOutput = nullptr; // scaling output color (display res, must be a UAV)
    unsigned int colorOutputState = 0;
    void* depth = nullptr; // render res
    unsigned int depthState = 0;
    void* motionVectors = nullptr; // render res
    unsigned int motionVectorsState = 0;

    unsigned int renderWidth = 0;
    unsigned int renderHeight = 0;
    unsigned int displayWidth = 0;
    unsigned int displayHeight = 0;

    DlssQuality quality = DlssQuality::DLAA;
    bool colorIsHdr = false;
    bool depthInverted = false;
    bool reset = false; // break temporal history (camera cut, resize, mode/preset change, load)

    float mvecScaleX = -0.5f;
    float mvecScaleY = 0.5f;
    float jitterX = 0.0f; // pixel-space jitter applied to the projection this frame
    float jitterY = 0.0f;

    float cameraViewToClip[16] = {};
    float clipToCameraView[16] = {};
    float clipToPrevClip[16] = {};
    float prevClipToClip[16] = {};

    float cameraNear = 0.0f;
    float cameraFar = 0.0f;
    float cameraFovVertical = 0.0f; // radians
    float cameraAspect = 0.0f;
    float cameraPos[3] = {};
    float cameraRight[3] = {};
    float cameraUp[3] = {};
    float cameraForward[3] = {};

    float exposureScale = 1.0f;
    float preExposure = 1.0f;
};

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

    // Records the DLSS upscale onto inputs.commandList and returns true if it evaluated. No-op that
    // returns false unless IsReady() && IsRuntimeInitialized() && IsDlssSupported(). Streamline
    // recreates the feature internally when the render/display extent or quality changes. NOT thread
    // safe — call only from the render thread, and rebind your descriptor heaps afterward (SL runs
    // with eDisableCLStateTracking so it does not restore command-list state).
    bool Evaluate(const DlssFrameInputs& inputs);

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
    unsigned int m_evaluateFrameIndex = 0; // monotonic frame id fed to slGetNewFrameToken
};
