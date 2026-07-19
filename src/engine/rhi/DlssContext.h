#pragma once

#include <atomic>
#include <cstdint>
#include <map>
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

enum class DlssReconstructionFeature
{
    SuperResolution,
    RayReconstruction,
};

struct DlssExtent
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    bool operator==(const DlssExtent& other) const
    {
        return width == other.width && height == other.height;
    }
};

// S2-P4 active-allocation cache identity. Viewport remains part of the key because the SDK
// query itself is viewport-independent: Scene and Game own independent planned and active state.
struct DlssExtentRecommendationKey
{
    std::uint32_t viewportId = 0;
    DlssExtent outputExtent{};
    DlssReconstructionFeature feature = DlssReconstructionFeature::SuperResolution;
    DlssQuality quality = DlssQuality::DLAA;

    bool operator==(const DlssExtentRecommendationKey& other) const;
    bool operator<(const DlssExtentRecommendationKey& other) const;
};

struct DlssExtentRecommendation
{
    DlssExtent recommended{};
    DlssExtent minimum{};
    DlssExtent maximum{};
};

enum class DlssExtentPlanSource
{
    Sdk,
    ExplicitFallback,
};

// The only planned-extent object exposed by the reconstruction integration. S2-P2 introduced the
// recommendation; S2-P4 makes active per-viewport allocations and tag extents consume it.
struct DlssPlannedExtent
{
    DlssExtentRecommendationKey key{};
    DlssExtentRecommendation extent{};
    DlssExtentPlanSource source = DlssExtentPlanSource::ExplicitFallback;
    std::string fallbackReason = "not-planned";
    bool rrNoArbitraryDrs = false;

    bool IsValid() const;
    bool IsSdkRecommendation() const { return source == DlssExtentPlanSource::Sdk; }
};

// S2-P4 activation boundary shared by production and deterministic CPU tests. A plan may drive
// allocation only when its complete tuple matches the active viewport/output/feature/quality key.
// An empty extent is returned on contradiction and reason explains the rejected contract.
DlssExtent ResolveDlssActiveRenderExtent(
    const DlssPlannedExtent& plan,
    const DlssExtentRecommendationKey& activeKey,
    std::string& reason);

struct DlssExtentPlanLookup
{
    DlssPlannedExtent plan{};
    bool cacheHit = false;
};

// SDK-independent query/cache core used by DlssContext and deterministic CPU contract tests.
class DlssExtentRecommendationCache
{
public:
    using QueryFunction = bool(*)(
        void* userData,
        const DlssExtentRecommendationKey& key,
        DlssExtentRecommendation& recommendation,
        std::string& failureReason);

    DlssExtentPlanLookup Plan(
        const DlssExtentRecommendationKey& key,
        QueryFunction query,
        void* userData);
    void Erase(const DlssExtentRecommendationKey& key);
    void Clear() { m_entries.clear(); }
    std::size_t Size() const { return m_entries.size(); }

private:
    std::map<DlssExtentRecommendationKey, DlssPlannedExtent> m_entries;
};

// DLSS-RR model preset (D4 experiment, devdoc/dxr/pt/rr-gi-diagnosis.md). Maps to sl::DLSSDPreset
// inside DlssContext.cpp. Presets are plain fields on DLSSDOptions, which we push every frame
// before evaluate — Streamline hot-swaps the model when the preset changes (one-frame hitch at
// most; no feature re-creation or app restart needed), so this is safe to A/B live from the UI.
enum class DlssRrPreset
{
    Default = 0,    // driver/SDK default model
    TransformerD,   // ePresetD — "Default model (transformer)"
    TransformerE,   // ePresetE — "Latest transformer model"
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
    bool motionVectorsDilated = false;
    // False asks Streamline to reconstruct camera motion from depth + clipToPrevClip. The supplied
    // motion texture must then contain object-only motion (the diagnostic path supplies zeroes).
    bool cameraMotionIncluded = true;

    // Exact S2-P2 recommendation that owns this evaluation's S2-P4 allocation and tag extents.
    DlssPlannedExtent extentPlan{};

    unsigned int renderWidth = 0;
    unsigned int renderHeight = 0;
    unsigned int displayWidth = 0;
    unsigned int displayHeight = 0;
    // Stable Streamline viewport identity. Concurrent editor viewports must never share it.
    std::uint32_t viewportId = 0;

    DlssQuality quality = DlssQuality::DLAA;
    bool colorIsHdr = false;
    bool depthInverted = false;
    bool reset = false; // break temporal history (camera cut, resize, mode/preset change, load)
    float mvecScaleX = -0.5f; // NDC normalization; SL multiplies by render width internally
    float mvecScaleY = 0.5f;  // Y-flip for texture space; SL multiplies by render height internally
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

    // Reconstruction guidance for ordinary DLSS only. DlssContext intentionally does not copy
    // these fields into RR options. They are never authored display exposure.
    float exposureScale = 1.0f;
    float preExposure = 1.0f;
    float sharpness = 0.0f; // [0,1]; 0 disables NGX sharpening (deprecated in SL but still honored)

    // Ray Reconstruction (devdoc/dxr/dlss-rr.md). When true, Evaluate() runs kFeatureDLSS_RR
    // instead of Super Resolution: it denoises the raw RT signal in colorInput using the guides
    // below (which MUST be non-null), then upscales/AAs — replacing NRD + the SR model in one pass.
    bool useRayReconstruction = false;
    DlssRrPreset rrPreset = DlssRrPreset::Default; // D4: RR model preset, hot-swappable per frame
    void* diffuseAlbedo = nullptr;   // kBufferTypeAlbedo         = albedo * (1 - metallic)
    unsigned int diffuseAlbedoState = 0;
    void* specularAlbedo = nullptr;  // kBufferTypeSpecularAlbedo = F0
    unsigned int specularAlbedoState = 0;
    void* normalRoughness = nullptr; // kBufferTypeNormalRoughness (PACKED): normal.rgb + roughness.a
    unsigned int normalRoughnessState = 0;
    // Optional (RR4): kBufferTypeSpecularHitDistance — raw reflection ray length in world units.
    // Tagged only when non-null (reflections on); RR runs without it otherwise. Sharpens reflections.
    void* specularHitDistance = nullptr;
    unsigned int specularHitDistanceState = 0;
    // Optional RR guide: dense specular-domain motion in the same NDC convention as main motion.
    void* specularMotionVectors = nullptr;
    unsigned int specularMotionVectorsState = 0;
    float worldToCameraView[16] = {}; // DLSSDOptions requires the view + inverse-view matrices
    float cameraViewToWorld[16] = {};
};

// Application-frame identity passed explicitly to every Streamline evaluation. The native token is
// owned by Streamline; this value only carries the borrowed handle for the current application
// frame. Viewport identity remains a separate DlssFrameInputs field.
struct DlssFrameToken
{
    void* native = nullptr;
    std::uint32_t frameIndex = 0;

    bool IsValid() const { return native != nullptr; }
};

// Small SDK-independent cadence state used by DlssContext and its CPU contract tests. BeginFrame is
// the only operation that advances identity; reading the current token for skipped, reordered, or
// failed evaluations cannot consume it.
class DlssFrameTokenState
{
public:
    using AcquireFunction = bool(*)(
        void* userData,
        std::uint32_t requestedFrameIndex,
        void*& nativeToken,
        std::uint32_t& actualFrameIndex);

    DlssFrameToken BeginFrame(AcquireFunction acquire, void* userData);
    DlssFrameToken Current() const { return m_current; }

private:
    std::uint32_t m_nextFrameIndex = 0;
    DlssFrameToken m_current{};
};

// NVIDIA DLSS via Streamline (devdoc/rendering/dlss-super-resolution.md).
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
    // DLSS Ray Reconstruction (kFeatureDLSS_RR) support on this adapter (devdoc/dxr/dlss-rr.md).
    // Probed alongside Super Resolution; RR consumers gate on this in addition to IsReady().
    bool IsRrSupported() const { return m_rrSupported.load(std::memory_order_acquire); }

    // Thread-safe snapshot of the human-readable status (worker updates it as it progresses).
    std::string StatusString() const;

    // Acquire and cache exactly one Streamline token for this application BeginFrame. Must be
    // called on the render thread before any Scene/Game DLSS or RR evaluation for the frame.
    void BeginFrame();
    DlssFrameToken CurrentFrameToken() const { return m_frameTokenState.Current(); }

    // Records the DLSS upscale onto inputs.commandList and returns true if it evaluated. No-op that
    // returns false unless IsReady() && IsRuntimeInitialized() && IsDlssSupported(). Streamline
    // recreates the feature internally when the render/display extent or quality changes. NOT thread
    // safe — call only from the render thread, and rebind your descriptor heaps afterward (SL runs
    // with eDisableCLStateTracking so it does not restore command-list state).
    bool Evaluate(const DlssFrameToken& frameToken, const DlssFrameInputs& inputs);

    // Query/cache the SDK's optimal render extent and supported range. This is planning only in
    // S2-P4: this recommendation owns allocation, tag extents, and motion/depth scale inputs.
    DlssPlannedExtent PlanReconstructionExtent(const DlssExtentRecommendationKey& key);
    void ClearPlannedExtentCache();

    // Releases Streamline's lazy per-viewport allocations for both SR and Ray Reconstruction.
    // The caller must flush every command list that could contain an evaluation first.
    void ReleaseViewportResources(std::uint32_t viewportId);

    // Upgrades the native swapchain to Streamline's DXGI proxy so Present/ResizeBuffers run
    // presentCommon() + GC (required for dynamic-load integrations; slHookPresent is not exported
    // from sl.interposer.dll). Call from the render thread once IsRuntimeInitialized(); returns
    // false until SL is ready or if upgrade fails. *swapChain is replaced on success.
    bool UpgradeSwapChain(void** swapChain);

    // True after slUpgradeInterface replaced the native swapchain with Streamline's proxy.
    // GfxContext::Shutdown must Detach its ComPtr after slShutdown (SL owns the proxy lifetime).
    bool IsSwapChainUpgraded() const { return m_swapChainUpgraded.load(std::memory_order_acquire); }

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
    std::atomic<bool> m_rrSupported{false}; // slIsFeatureSupported(DLSS_RR) == eOk for this adapter
    std::atomic<bool> m_swapChainUpgraded{false};

    mutable std::mutex m_statusMutex;
    mutable std::mutex m_extentPlanMutex;
    DlssExtentRecommendationCache m_extentPlanCache;
    std::string m_status = "DLSS: initializing…";
    void* m_interposer = nullptr; // HMODULE for sl.interposer.dll (dynamic load)
    DlssFrameTokenState m_frameTokenState;
};
