#include "engine/rhi/DlssContext.h"

#include "engine/platform/EngineLog.h"

#ifdef GAME_ENGINE_ENABLE_DLSS
#include <windows.h>

#include <d3d12.h>
#include <dxgi.h>

#include <sl.h>
#include <sl_dlss.h>
#include <sl_dlss_d.h> // DLSS Ray Reconstruction (kFeatureDLSS_RR), devdoc/dxr-dlss-rr.md

#include <cstring>
#include <algorithm>
#include <iterator>
#include <vector>

namespace
{
// Resolved from sl.interposer.dll at runtime (see header rationale for not static-linking).
// Written and reset only by the init worker / Shutdown (which joins first), so no locking needed.
PFun_slInit* g_slInit = nullptr;
PFun_slShutdown* g_slShutdown = nullptr;
PFun_slSetD3DDevice* g_slSetD3DDevice = nullptr;
PFun_slIsFeatureSupported* g_slIsFeatureSupported = nullptr;

// Core evaluate/tag/constants exports (all C exports from sl.interposer.dll).
PFun_slGetNewFrameToken* g_slGetNewFrameToken = nullptr;
PFun_slSetConstants* g_slSetConstants = nullptr;
PFun_slSetTagForFrame* g_slSetTagForFrame = nullptr;
PFun_slEvaluateFeature* g_slEvaluateFeature = nullptr;
PFun_slGetFeatureFunction* g_slGetFeatureFunction = nullptr;
PFun_slUpgradeInterface* g_slUpgradeInterface = nullptr;

// DLSS feature functions (obtained via slGetFeatureFunction AFTER the device is set).
PFun_slDLSSSetOptions* g_slDLSSSetOptions = nullptr;
PFun_slDLSSGetOptimalSettings* g_slDLSSGetOptimalSettings = nullptr;
// DLSS-RR (Ray Reconstruction) feature function (kFeatureDLSS_RR).
PFun_slDLSSDSetOptions* g_slDLSSDSetOptions = nullptr;

// Single viewport — the editor drives one scene view through DLSS at a time.
constexpr uint32_t kDlssViewport = 0;

// NVIDIA's public sample/development application id. It lets NGX/DLSS load in a dev context; a
// title shipping DLSS must swap this for an NVIDIA-issued application id (or engine id + project
// id). See ProgrammingGuide "Preferences".
constexpr uint32_t kApplicationId = 231313132;

void SlLogCallback(sl::LogType type, const char* msg)
{
    if (msg == nullptr)
    {
        return;
    }
    switch (type)
    {
    case sl::LogType::eError:
        EngineLog::Error("dlss", msg);
        break;
    case sl::LogType::eWarn:
        EngineLog::Warn("dlss", msg);
        break;
    default:
        EngineLog::Info("dlss", msg);
        break;
    }
}

const char* ResultToString(sl::Result r)
{
    switch (r)
    {
    case sl::Result::eOk: return "ok";
    case sl::Result::eErrorDriverOutOfDate: return "driver out of date";
    case sl::Result::eErrorOSOutOfDate: return "OS out of date";
    case sl::Result::eErrorOSDisabledHWS: return "GPU hardware scheduling disabled";
    case sl::Result::eErrorNoSupportedAdapterFound: return "no supported adapter (needs NVIDIA RTX)";
    case sl::Result::eErrorAdapterNotSupported: return "adapter not supported";
    case sl::Result::eErrorNoPlugins: return "SL plugins not found next to the executable";
    case sl::Result::eErrorNGXFailed: return "NGX init failed";
    case sl::Result::eErrorFeatureNotSupported: return "feature not supported";
    case sl::Result::eErrorMissingOrInvalidAPI: return "missing/invalid API";
    default: return "unsupported/unknown error";
    }
}
} // namespace
#endif // GAME_ENGINE_ENABLE_DLSS

DlssContext& DlssContext::Get()
{
    static DlssContext instance;
    return instance;
}

void DlssContext::SetStatus(std::string status)
{
    std::lock_guard<std::mutex> lock(m_statusMutex);
    m_status = std::move(status);
}

std::string DlssContext::StatusString() const
{
    std::lock_guard<std::mutex> lock(m_statusMutex);
    return m_status;
}

void DlssContext::BeginAsyncInitialize(ID3D12Device* device, IDXGIAdapter* adapter)
{
#ifndef GAME_ENGINE_ENABLE_DLSS
    (void)device;
    (void)adapter;
    m_ready.store(true, std::memory_order_release);
    SetStatus("DLSS: disabled at build time");
#else
    bool expected = false;
    if (!m_started.compare_exchange_strong(expected, true))
    {
        return; // already started
    }
    if (device == nullptr || adapter == nullptr)
    {
        m_ready.store(true, std::memory_order_release);
        SetStatus("DLSS: no device");
        return;
    }

    // Keep the device/adapter alive for the duration of the worker (they outlive it anyway, but be
    // explicit). Released at the end of RunInitialize.
    device->AddRef();
    adapter->AddRef();
    m_worker = std::thread(&DlssContext::RunInitialize, this, device, adapter);
#endif
}

void DlssContext::RunInitialize(ID3D12Device* device, IDXGIAdapter* adapter)
{
#ifdef GAME_ENGINE_ENABLE_DLSS
    HMODULE hmod = ::LoadLibraryW(L"sl.interposer.dll");
    if (hmod == nullptr)
    {
        SetStatus("DLSS: sl.interposer.dll not found next to the executable");
        EngineLog::Warn("dlss", "sl.interposer.dll not found next to the executable");
        device->Release();
        adapter->Release();
        m_ready.store(true, std::memory_order_release);
        return;
    }
    m_interposer = hmod;

    bool resolved = true;
#define SL_RESOLVE(fn)                                                                             \
    g_##fn = reinterpret_cast<PFun_##fn*>(::GetProcAddress(hmod, #fn));                             \
    if (g_##fn == nullptr)                                                                          \
    {                                                                                              \
        SetStatus("DLSS: sl.interposer.dll is missing export " #fn);                               \
        EngineLog::Warn("dlss", "sl.interposer.dll is missing export " #fn);                       \
        resolved = false;                                                                          \
    }

    SL_RESOLVE(slInit)
    SL_RESOLVE(slShutdown)
    SL_RESOLVE(slSetD3DDevice)
    SL_RESOLVE(slIsFeatureSupported)
    SL_RESOLVE(slGetNewFrameToken)
    SL_RESOLVE(slSetConstants)
    SL_RESOLVE(slSetTagForFrame)
    SL_RESOLVE(slEvaluateFeature)
    SL_RESOLVE(slGetFeatureFunction)
    SL_RESOLVE(slUpgradeInterface)
#undef SL_RESOLVE

    if (!resolved)
    {
        ::FreeLibrary(hmod);
        m_interposer = nullptr;
        device->Release();
        adapter->Release();
        m_ready.store(true, std::memory_order_release);
        return;
    }

    // Load both DLSS Super Resolution and Ray Reconstruction plugins. RR (kFeatureDLSS_RR) needs
    // sl.dlss_d.dll + nvngx_dlssd.dll next to the exe (shipped by CMake); if absent SL maps+ignores
    // it and the RR probe below simply reports unsupported.
    static const sl::Feature kFeaturesToLoad[] = {sl::kFeatureDLSS, sl::kFeatureDLSS_RR};

    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = sl::LogLevel::eDefault;
    pref.logMessageCallback = &SlLogCallback;
    pref.featuresToLoad = kFeaturesToLoad;
    pref.numFeaturesToLoad = static_cast<uint32_t>(std::size(kFeaturesToLoad));
    // We restore command-list state ourselves after slEvaluateFeature (default SL behavior).
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking
        | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.applicationId = kApplicationId;

    const sl::Result initResult = g_slInit(pref, sl::kSDKVersion);
    if (initResult != sl::Result::eOk)
    {
        SetStatus(std::string("DLSS: slInit failed (") + ResultToString(initResult) + ")");
        EngineLog::Warn("dlss", StatusString());
        device->Release();
        adapter->Release();
        m_ready.store(true, std::memory_order_release);
        return;
    }
    m_initialized.store(true, std::memory_order_release);
    EngineLog::Info("dlss", "Streamline initialized (SL 2.12).");

    if (g_slSetD3DDevice(device) != sl::Result::eOk)
    {
        SetStatus("DLSS: slSetD3DDevice failed");
        EngineLog::Warn("dlss", "slSetD3DDevice failed");
        device->Release();
        adapter->Release();
        m_ready.store(true, std::memory_order_release);
        return;
    }

    // DLSS feature functions can only be resolved once a device is bound (slGetFeatureFunction
    // contract). Missing ones simply disable Evaluate() — support probing below still runs.
    if (g_slGetFeatureFunction != nullptr)
    {
        void* fn = nullptr;
        if (g_slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", fn) == sl::Result::eOk)
        {
            g_slDLSSSetOptions = reinterpret_cast<PFun_slDLSSSetOptions*>(fn);
        }
        fn = nullptr;
        if (g_slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", fn) == sl::Result::eOk)
        {
            g_slDLSSGetOptimalSettings = reinterpret_cast<PFun_slDLSSGetOptimalSettings*>(fn);
        }
        // Ray Reconstruction set-options (kFeatureDLSS_RR). Missing simply disables the RR path.
        fn = nullptr;
        if (g_slGetFeatureFunction(sl::kFeatureDLSS_RR, "slDLSSDSetOptions", fn) == sl::Result::eOk)
        {
            g_slDLSSDSetOptions = reinterpret_cast<PFun_slDLSSDSetOptions*>(fn);
        }
    }

    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(adapter->GetDesc(&desc)))
    {
        sl::AdapterInfo adapterInfo{};
        adapterInfo.deviceLUID = reinterpret_cast<uint8_t*>(&desc.AdapterLuid);
        adapterInfo.deviceLUIDSizeInBytes = sizeof(LUID);

        const sl::Result supportResult = g_slIsFeatureSupported(sl::kFeatureDLSS, adapterInfo);
        if (supportResult == sl::Result::eOk)
        {
            m_supported.store(true, std::memory_order_release);
            EngineLog::Info("dlss", "DLSS Super Resolution is supported on this adapter.");
        }
        else
        {
            EngineLog::Info(
                "dlss",
                std::string("DLSS Super Resolution unavailable (") + ResultToString(supportResult) + ")");
        }

        // Ray Reconstruction probe (devdoc/dxr-dlss-rr.md). Independent of SR support.
        const sl::Result rrSupportResult = g_slIsFeatureSupported(sl::kFeatureDLSS_RR, adapterInfo);
        if (rrSupportResult == sl::Result::eOk)
        {
            m_rrSupported.store(true, std::memory_order_release);
            EngineLog::Info("dlss", "DLSS Ray Reconstruction is supported on this adapter.");
        }
        else
        {
            EngineLog::Info(
                "dlss",
                std::string("DLSS Ray Reconstruction unavailable (") + ResultToString(rrSupportResult)
                    + ")");
        }

        const std::string srStatus =
            m_supported.load(std::memory_order_acquire) ? "supported" : "unavailable";
        const std::string rrStatus =
            m_rrSupported.load(std::memory_order_acquire) ? "supported" : "unavailable";
        SetStatus("DLSS: " + srStatus + " | Ray Reconstruction: " + rrStatus);
    }
    else
    {
        SetStatus("DLSS: could not read adapter description");
        EngineLog::Warn("dlss", "could not read adapter description");
    }

    device->Release();
    adapter->Release();
    m_ready.store(true, std::memory_order_release);
#else
    (void)device;
    (void)adapter;
#endif
}

void DlssContext::Shutdown()
{
    if (m_worker.joinable())
    {
        m_worker.join();
    }
#ifdef GAME_ENGINE_ENABLE_DLSS
    if (m_initialized.load(std::memory_order_acquire) && g_slShutdown != nullptr)
    {
        g_slShutdown();
    }
    m_initialized.store(false, std::memory_order_release);
    m_supported.store(false, std::memory_order_release);
    m_rrSupported.store(false, std::memory_order_release);
    m_swapChainUpgraded.store(false, std::memory_order_release);
    g_slInit = nullptr;
    g_slShutdown = nullptr;
    g_slSetD3DDevice = nullptr;
    g_slIsFeatureSupported = nullptr;
    g_slGetNewFrameToken = nullptr;
    g_slSetConstants = nullptr;
    g_slSetTagForFrame = nullptr;
    g_slEvaluateFeature = nullptr;
    g_slGetFeatureFunction = nullptr;
    g_slDLSSSetOptions = nullptr;
    g_slDLSSGetOptimalSettings = nullptr;
    g_slDLSSDSetOptions = nullptr;
    g_slUpgradeInterface = nullptr;
    if (m_interposer != nullptr)
    {
        ::FreeLibrary(static_cast<HMODULE>(m_interposer));
        m_interposer = nullptr;
    }
    SetStatus("DLSS: shut down");
#endif
}

#ifdef GAME_ENGINE_ENABLE_DLSS
namespace
{
sl::DLSSMode ToDlssMode(DlssQuality quality)
{
    switch (quality)
    {
    case DlssQuality::DLAA: return sl::DLSSMode::eDLAA;
    case DlssQuality::Quality: return sl::DLSSMode::eMaxQuality;
    case DlssQuality::Balanced: return sl::DLSSMode::eBalanced;
    case DlssQuality::Performance: return sl::DLSSMode::eMaxPerformance;
    case DlssQuality::UltraPerformance: return sl::DLSSMode::eUltraPerformance;
    default: return sl::DLSSMode::eDLAA;
    }
}

// The engine hands us 16-float glm matrices (column-major storage, column-vector convention). That
// byte layout is identical to Streamline's float4x4 (row-major storage, row-vector convention):
// SL.row[i] == glm column i, which also transposes the operator into row-vector form. So a straight
// 16-float copy yields the correct matrix on the SL side — no explicit transpose needed.
void CopyMatrix(sl::float4x4& dst, const float (&src)[16])
{
    std::memcpy(&dst[0].x, src, sizeof(float) * 16);
}

sl::Resource MakeTex(void* native, unsigned int state, unsigned int width, unsigned int height)
{
    sl::Resource resource(sl::ResourceType::eTex2d, native, static_cast<uint32_t>(state));
    resource.width = width;
    resource.height = height;
    return resource;
}
} // namespace
#endif // GAME_ENGINE_ENABLE_DLSS

bool DlssContext::Evaluate(const DlssFrameInputs& inputs)
{
#ifndef GAME_ENGINE_ENABLE_DLSS
    (void)inputs;
    return false;
#else
    if (!IsReady() || !IsRuntimeInitialized())
    {
        return false;
    }
    if (inputs.useRayReconstruction)
    {
        if (!IsRrSupported() || g_slDLSSDSetOptions == nullptr || inputs.diffuseAlbedo == nullptr
            || inputs.specularAlbedo == nullptr || inputs.normalRoughness == nullptr)
        {
            return false;
        }
    }
    else if (!IsDlssSupported() || g_slDLSSSetOptions == nullptr)
    {
        return false;
    }
    if (g_slGetNewFrameToken == nullptr || g_slSetConstants == nullptr
        || g_slSetTagForFrame == nullptr || g_slEvaluateFeature == nullptr)
    {
        return false;
    }
    if (inputs.commandList == nullptr || inputs.colorInput == nullptr
        || inputs.colorOutput == nullptr || inputs.depth == nullptr
        || inputs.motionVectors == nullptr)
    {
        return false;
    }

    auto* cmdList = static_cast<sl::CommandBuffer*>(inputs.commandList);
    const sl::ViewportHandle viewport(kDlssViewport);

    const uint32_t frameIndex = m_evaluateFrameIndex++;
    sl::FrameToken* frameToken = nullptr;
    if (g_slGetNewFrameToken(frameToken, &frameIndex) != sl::Result::eOk || frameToken == nullptr)
    {
        return false;
    }

    // Tag the four buffers DLSS SR consumes/produces. eValidUntilPresent is the recommended default;
    // the generating GPU work (tonemap/scene passes) is already recorded on cmdList before this call.
    sl::Resource colorIn =
        MakeTex(inputs.colorInput, inputs.colorInputState, inputs.renderWidth, inputs.renderHeight);
    sl::Resource colorOut = MakeTex(
        inputs.colorOutput, inputs.colorOutputState, inputs.displayWidth, inputs.displayHeight);
    sl::Resource depth =
        MakeTex(inputs.depth, inputs.depthState, inputs.renderWidth, inputs.renderHeight);
    sl::Resource mvec = MakeTex(
        inputs.motionVectors, inputs.motionVectorsState, inputs.renderWidth, inputs.renderHeight);

    // Guide resources must outlive the slSetTagForFrame call (the tags hold pointers to them).
    sl::Resource diffuseAlbedo{};
    sl::Resource specularAlbedo{};
    sl::Resource normalRoughness{};
    sl::Resource specularHitDistance{};

    std::vector<sl::ResourceTag> tags;
    tags.reserve(8);
    tags.emplace_back(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent);
    tags.emplace_back(
        &mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent);
    tags.emplace_back(
        &colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent);
    tags.emplace_back(
        &colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent);
    if (inputs.useRayReconstruction)
    {
        // RR material guides (devdoc/dxr-dlss-rr.md). normalRoughness is PACKED (DLSSDNormalRoughnessMode::ePacked).
        diffuseAlbedo = MakeTex(
            inputs.diffuseAlbedo, inputs.diffuseAlbedoState, inputs.renderWidth, inputs.renderHeight);
        specularAlbedo = MakeTex(
            inputs.specularAlbedo, inputs.specularAlbedoState, inputs.renderWidth, inputs.renderHeight);
        normalRoughness = MakeTex(
            inputs.normalRoughness, inputs.normalRoughnessState, inputs.renderWidth, inputs.renderHeight);
        tags.emplace_back(
            &diffuseAlbedo, sl::kBufferTypeAlbedo, sl::ResourceLifecycle::eValidUntilPresent);
        tags.emplace_back(
            &specularAlbedo, sl::kBufferTypeSpecularAlbedo, sl::ResourceLifecycle::eValidUntilPresent);
        tags.emplace_back(
            &normalRoughness, sl::kBufferTypeNormalRoughness, sl::ResourceLifecycle::eValidUntilPresent);
        // Optional spec hit-distance guide (RR4): present only when reflections ran this frame.
        if (inputs.specularHitDistance != nullptr)
        {
            specularHitDistance = MakeTex(
                inputs.specularHitDistance, inputs.specularHitDistanceState,
                inputs.renderWidth, inputs.renderHeight);
            tags.emplace_back(
                &specularHitDistance, sl::kBufferTypeSpecularHitDistance,
                sl::ResourceLifecycle::eValidUntilPresent);
        }
    }
    if (g_slSetTagForFrame(*frameToken, viewport, tags.data(), static_cast<uint32_t>(tags.size()), cmdList)
        != sl::Result::eOk)
    {
        return false;
    }

    sl::Constants consts{};
    CopyMatrix(consts.cameraViewToClip, inputs.cameraViewToClip);
    CopyMatrix(consts.clipToCameraView, inputs.clipToCameraView);
    CopyMatrix(consts.clipToPrevClip, inputs.clipToPrevClip);
    CopyMatrix(consts.prevClipToClip, inputs.prevClipToClip);
    consts.jitterOffset = sl::float2(inputs.jitterX, inputs.jitterY);
    consts.mvecScale = sl::float2(inputs.mvecScaleX, inputs.mvecScaleY);
    consts.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    consts.cameraPos = sl::float3(inputs.cameraPos[0], inputs.cameraPos[1], inputs.cameraPos[2]);
    consts.cameraUp = sl::float3(inputs.cameraUp[0], inputs.cameraUp[1], inputs.cameraUp[2]);
    consts.cameraRight =
        sl::float3(inputs.cameraRight[0], inputs.cameraRight[1], inputs.cameraRight[2]);
    consts.cameraFwd =
        sl::float3(inputs.cameraForward[0], inputs.cameraForward[1], inputs.cameraForward[2]);
    consts.cameraNear = inputs.cameraNear;
    consts.cameraFar = inputs.cameraFar;
    consts.cameraFOV = inputs.cameraFovVertical;
    consts.cameraAspectRatio = inputs.cameraAspect;
    consts.depthInverted = inputs.depthInverted ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    consts.cameraMotionIncluded = sl::Boolean::eTrue;
    consts.orthographicProjection = sl::Boolean::eFalse;
    consts.motionVectors3D = sl::Boolean::eFalse;
    consts.motionVectorsDilated = sl::Boolean::eFalse;
    consts.motionVectorsJittered = sl::Boolean::eFalse;
    consts.reset = inputs.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    if (g_slSetConstants(consts, *frameToken, viewport) != sl::Result::eOk)
    {
        return false;
    }

    sl::Feature feature = sl::kFeatureDLSS;
    if (inputs.useRayReconstruction)
    {
        sl::DLSSDOptions rrOptions{};
        rrOptions.mode = ToDlssMode(inputs.quality);
        rrOptions.outputWidth = inputs.displayWidth;
        rrOptions.outputHeight = inputs.displayHeight;
        rrOptions.colorBuffersHDR = inputs.colorIsHdr ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        rrOptions.preExposure = inputs.preExposure;
        rrOptions.exposureScale = inputs.exposureScale;
        rrOptions.sharpness = std::clamp(inputs.sharpness, 0.0f, 1.0f);
        rrOptions.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked;
        CopyMatrix(rrOptions.worldToCameraView, inputs.worldToCameraView);
        CopyMatrix(rrOptions.cameraViewToWorld, inputs.cameraViewToWorld);
        if (g_slDLSSDSetOptions(viewport, rrOptions) != sl::Result::eOk)
        {
            return false;
        }
        feature = sl::kFeatureDLSS_RR;
    }
    else
    {
        sl::DLSSOptions options{};
        options.mode = ToDlssMode(inputs.quality);
        options.outputWidth = inputs.displayWidth;
        options.outputHeight = inputs.displayHeight;
        options.colorBuffersHDR = inputs.colorIsHdr ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        options.preExposure = inputs.preExposure;
        options.exposureScale = inputs.exposureScale;
        options.sharpness = std::clamp(inputs.sharpness, 0.0f, 1.0f);
        if (g_slDLSSSetOptions(viewport, options) != sl::Result::eOk)
        {
            return false;
        }
    }

    const sl::BaseStructure* evalInputs[] = {&viewport};
    const sl::Result evalResult = g_slEvaluateFeature(
        feature, *frameToken, evalInputs, static_cast<uint32_t>(std::size(evalInputs)),
        cmdList);
    if (evalResult != sl::Result::eOk)
    {
        static bool loggedOnce = false;
        if (!loggedOnce)
        {
            loggedOnce = true;
            EngineLog::Warn(
                "dlss",
                std::string("slEvaluateFeature failed (") + ResultToString(evalResult) + ")");
        }
        return false;
    }
    return true;
#endif
}

bool DlssContext::UpgradeSwapChain(void** swapChain)
{
#ifndef GAME_ENGINE_ENABLE_DLSS
    (void)swapChain;
    return false;
#else
    if (!IsRuntimeInitialized() || g_slUpgradeInterface == nullptr || swapChain == nullptr
        || *swapChain == nullptr)
    {
        return false;
    }

    if (m_swapChainUpgraded.load(std::memory_order_acquire))
    {
        return true;
    }

    void* const before = *swapChain;
    const sl::Result upgradeResult = g_slUpgradeInterface(swapChain);
    if (upgradeResult != sl::Result::eOk)
    {
        static bool loggedOnce = false;
        if (!loggedOnce)
        {
            loggedOnce = true;
            EngineLog::Warn(
                "dlss",
                std::string("slUpgradeInterface(swapchain) failed (") + ResultToString(upgradeResult)
                    + "); Streamline GC will not run");
        }
        return false;
    }

    m_swapChainUpgraded.store(true, std::memory_order_release);
    if (*swapChain != before)
    {
        EngineLog::Info("dlss", "Swapchain upgraded to Streamline proxy (presentCommon/GC enabled)");
    }
    return true;
#endif
}
