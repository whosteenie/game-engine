#include "engine/rhi/DlssContext.h"

#include "engine/platform/system/BackgroundWork.h"

#include "engine/platform/diagnostics/FrameDiagnostics.h"

#include "engine/rhi/GfxContext.h"

#include "engine/platform/diagnostics/EngineLog.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <sstream>
#include <tuple>

#ifdef GAME_ENGINE_ENABLE_DLSS
#include <windows.h>

#include <d3d12.h>
#include <dxgi.h>

#include <sl.h>
#include <sl_dlss.h>
#include <sl_dlss_d.h> // DLSS Ray Reconstruction (kFeatureDLSS_RR), devdoc/dxr/dlss-rr.md

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
PFun_slFreeResources* g_slFreeResources = nullptr;
PFun_slGetFeatureFunction* g_slGetFeatureFunction = nullptr;
PFun_slUpgradeInterface* g_slUpgradeInterface = nullptr;

// DLSS feature functions (obtained via slGetFeatureFunction AFTER the device is set).
PFun_slDLSSSetOptions* g_slDLSSSetOptions = nullptr;
PFun_slDLSSGetOptimalSettings* g_slDLSSGetOptimalSettings = nullptr;
// DLSS-RR (Ray Reconstruction) feature function (kFeatureDLSS_RR).
PFun_slDLSSDSetOptions* g_slDLSSDSetOptions = nullptr;
PFun_slDLSSDGetOptimalSettings* g_slDLSSDGetOptimalSettings = nullptr;

// Single viewport — the editor drives one scene view through DLSS at a time.

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

    // Loading both kFeatureDLSS and kFeatureDLSS_RR makes Streamline warn about duplicate hooks
    // and compute kernels. That is expected and does not indicate double-init on our side.
    if (type == sl::LogType::eWarn)
    {
        if (std::strstr(msg, "DUPLICATED") != nullptr || std::strstr(msg, "already created") != nullptr)
        {
            return;
        }
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
    case sl::Result::eErrorInvalidParameter: return "invalid parameter";
    case sl::Result::eErrorFeatureNotSupported: return "feature not supported";
    case sl::Result::eErrorMissingOrInvalidAPI: return "missing/invalid API";
    default: return "unsupported/unknown error";
    }
}
} // namespace
#endif // GAME_ENGINE_ENABLE_DLSS

namespace
{
const char* ExtentFeatureName(const DlssReconstructionFeature feature)
{
    return feature == DlssReconstructionFeature::RayReconstruction ? "rr" : "dlss";
}

const char* ExtentQualityName(const DlssQuality quality)
{
    switch (quality)
    {
    case DlssQuality::DLAA: return "dlaa";
    case DlssQuality::Quality: return "quality";
    case DlssQuality::Balanced: return "balanced";
    case DlssQuality::Performance: return "performance";
    case DlssQuality::UltraPerformance: return "ultra-performance";
    }
    return "unknown";
}

float LegacyFallbackScale(const DlssQuality quality)
{
    switch (quality)
    {
    case DlssQuality::Quality: return 0.667f;
    case DlssQuality::Balanced: return 0.58f;
    case DlssQuality::Performance: return 0.5f;
    case DlssQuality::UltraPerformance: return 0.333f;
    case DlssQuality::DLAA: return 1.0f;
    }
    return 1.0f;
}

DlssPlannedExtent MakeExplicitFallback(
    const DlssExtentRecommendationKey& key,
    std::string reason)
{
    DlssPlannedExtent plan{};
    plan.key = key;
    plan.source = DlssExtentPlanSource::ExplicitFallback;
    plan.fallbackReason = reason.empty() ? "query-failed-without-reason" : std::move(reason);
    plan.rrNoArbitraryDrs = key.feature == DlssReconstructionFeature::RayReconstruction;

    // RR never invents a DRS ratio. If its recommendation query fails, native is the only planned
    // fallback. Ordinary SR retains the legacy active ratio, but labels it explicitly as fallback;
    // it is never represented as an SDK recommendation. Active allocation remains separate.
    const float scale = key.feature == DlssReconstructionFeature::RayReconstruction
        ? 1.0f
        : LegacyFallbackScale(key.quality);
    const auto scaled = [scale](const std::uint32_t value) {
        return std::max<std::uint32_t>(
            1u,
            static_cast<std::uint32_t>(std::lround(static_cast<double>(value) * scale)));
    };
    plan.extent.recommended = {scaled(key.outputExtent.width), scaled(key.outputExtent.height)};
    plan.extent.minimum = plan.extent.recommended;
    plan.extent.maximum = plan.extent.recommended;
    return plan;
}

bool IsValidSdkRecommendation(
    const DlssExtentRecommendationKey& key,
    const DlssExtentRecommendation& recommendation,
    std::string& reason)
{
    const DlssExtent& value = recommendation.recommended;
    const DlssExtent& minimum = recommendation.minimum;
    const DlssExtent& maximum = recommendation.maximum;
    if (key.outputExtent.width == 0 || key.outputExtent.height == 0)
    {
        reason = "invalid-output-extent";
        return false;
    }
    if (value.width == 0 || value.height == 0 || minimum.width == 0 || minimum.height == 0
        || maximum.width == 0 || maximum.height == 0)
    {
        reason = "sdk-returned-zero-extent";
        return false;
    }
    if (minimum.width > value.width || value.width > maximum.width
        || minimum.height > value.height || value.height > maximum.height)
    {
        reason = "sdk-recommendation-outside-returned-range";
        return false;
    }
    if (key.quality == DlssQuality::DLAA && !(value == key.outputExtent))
    {
        reason = "sdk-dlaa-recommendation-is-not-native";
        return false;
    }
    return true;
}
} // namespace

bool DlssExtentRecommendationKey::operator==(const DlssExtentRecommendationKey& other) const
{
    return viewportId == other.viewportId && outputExtent == other.outputExtent
        && feature == other.feature && quality == other.quality;
}

bool DlssExtentRecommendationKey::operator<(const DlssExtentRecommendationKey& other) const
{
    return std::tie(viewportId, outputExtent.width, outputExtent.height, feature, quality)
        < std::tie(
            other.viewportId,
            other.outputExtent.width,
            other.outputExtent.height,
            other.feature,
            other.quality);
}

bool DlssPlannedExtent::IsValid() const
{
    return extent.recommended.width > 0 && extent.recommended.height > 0
        && extent.minimum.width > 0 && extent.minimum.height > 0
        && extent.maximum.width > 0 && extent.maximum.height > 0;
}

DlssExtent ResolveDlssActiveRenderExtent(
    const DlssPlannedExtent& plan,
    const DlssExtentRecommendationKey& activeKey,
    std::string& reason)
{
    reason.clear();
    if (!(plan.key == activeKey))
    {
        reason = "planned-tuple-does-not-match-active-tuple";
        return {};
    }
    if (!plan.IsValid())
    {
        reason = "planned-extent-is-invalid";
        return {};
    }
    const DlssExtent& render = plan.extent.recommended;
    if (render.width < plan.extent.minimum.width || render.width > plan.extent.maximum.width
        || render.height < plan.extent.minimum.height || render.height > plan.extent.maximum.height)
    {
        reason = "planned-extent-is-outside-supported-range";
        return {};
    }
    if (activeKey.quality == DlssQuality::DLAA && !(render == activeKey.outputExtent))
    {
        reason = "dlaa-active-extent-is-not-native";
        return {};
    }
    if (activeKey.feature == DlssReconstructionFeature::RayReconstruction)
    {
        if (!plan.rrNoArbitraryDrs)
        {
            reason = "rr-plan-does-not-enforce-no-arbitrary-drs";
            return {};
        }
        if (!plan.IsSdkRecommendation() && !(render == activeKey.outputExtent))
        {
            reason = "rr-fallback-is-not-native";
            return {};
        }
    }
    return render;
}

DlssExtentPlanLookup DlssExtentRecommendationCache::Plan(
    const DlssExtentRecommendationKey& key,
    const QueryFunction query,
    void* const userData)
{
    const auto found = m_entries.find(key);
    if (found != m_entries.end())
    {
        return {found->second, true};
    }

    DlssExtentRecommendation recommendation{};
    std::string failureReason;
    bool succeeded = query != nullptr && query(userData, key, recommendation, failureReason);
    if (succeeded)
    {
        succeeded = IsValidSdkRecommendation(key, recommendation, failureReason);
    }

    DlssPlannedExtent plan{};
    if (succeeded)
    {
        plan.key = key;
        plan.extent = recommendation;
        plan.source = DlssExtentPlanSource::Sdk;
        plan.fallbackReason.clear();
        plan.rrNoArbitraryDrs = key.feature == DlssReconstructionFeature::RayReconstruction;
    }
    else
    {
        plan = MakeExplicitFallback(
            key,
            query == nullptr ? "query-entrypoint-unavailable" : std::move(failureReason));
    }
    m_entries.emplace(key, plan);
    return {plan, false};
}

void DlssExtentRecommendationCache::Erase(const DlssExtentRecommendationKey& key)
{
    m_entries.erase(key);
}

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
    BackgroundWork::LowerCurrentThreadPriority();
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
    SL_RESOLVE(slFreeResources)
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
        fn = nullptr;
        if (g_slGetFeatureFunction(
                sl::kFeatureDLSS_RR, "slDLSSDGetOptimalSettings", fn) == sl::Result::eOk)
        {
            g_slDLSSDGetOptimalSettings =
                reinterpret_cast<PFun_slDLSSDGetOptimalSettings*>(fn);
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

        // Ray Reconstruction probe (devdoc/dxr/dlss-rr.md). Independent of SR support.
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
    ClearPlannedExtentCache();
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
    g_slFreeResources = nullptr;
    g_slGetFeatureFunction = nullptr;
    g_slDLSSSetOptions = nullptr;
    g_slDLSSGetOptimalSettings = nullptr;
    g_slDLSSDSetOptions = nullptr;
    g_slDLSSDGetOptimalSettings = nullptr;
    g_slUpgradeInterface = nullptr;
    if (m_interposer != nullptr)
    {
        ::FreeLibrary(static_cast<HMODULE>(m_interposer));
        m_interposer = nullptr;
    }
    SetStatus("DLSS: shut down");
#endif
}

void DlssContext::ReleaseViewportResources(const std::uint32_t viewportId)
{
#ifdef GAME_ENGINE_ENABLE_DLSS
    if (!IsRuntimeInitialized() || g_slFreeResources == nullptr)
    {
        return;
    }

    const auto releaseFeature = [&](const std::uint32_t id, const sl::Feature feature, const char* const label) {
        const sl::Result result = g_slFreeResources(feature, sl::ViewportHandle(id));
        // Streamline uses eErrorInvalidParameter to mean this viewport never created an instance
        // for the feature (for example SR when the project used RR). That is already the desired
        // released state and should not surface as a teardown warning.
        if (result != sl::Result::eOk && result != sl::Result::eErrorInvalidParameter)
        {
            EngineLog::Warn(
                "dlss",
                std::string("slFreeResources(") + label + ", viewport "
                    + std::to_string(id) + ") failed (" + ResultToString(result) + ")");
        }
    };

    constexpr std::uint32_t kOpticalTransmissionViewportBit = 0x80000000u;
    for (const std::uint32_t id : {viewportId, viewportId ^ kOpticalTransmissionViewportBit})
    {
        if (IsDlssSupported())
        {
            releaseFeature(id, sl::kFeatureDLSS, "DLSS");
        }
        if (IsRrSupported())
        {
            releaseFeature(id, sl::kFeatureDLSS_RR, "DLSS-RR");
        }
    }
#else
    (void)viewportId;
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
    if (native != nullptr)
    {
        const D3D12_RESOURCE_DESC desc = static_cast<ID3D12Resource*>(native)->GetDesc();
        resource.nativeFormat = static_cast<std::uint32_t>(desc.Format);
        resource.mipLevels = desc.MipLevels;
        resource.arrayLayers = desc.DepthOrArraySize;
    }
    return resource;
}

bool ValidateTaggedTexture(
    void* const native,
    const std::uint32_t expectedWidth,
    const std::uint32_t expectedHeight,
    const std::initializer_list<DXGI_FORMAT> allowedFormats,
    const char* const label,
    std::string& reason)
{
    if (native == nullptr)
    {
        reason = std::string(label) + "-resource-is-null";
        return false;
    }
    const D3D12_RESOURCE_DESC desc = static_cast<ID3D12Resource*>(native)->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D
        || desc.Width != expectedWidth || desc.Height != expectedHeight)
    {
        std::ostringstream message;
        message << label << "-resource-extent=" << desc.Width << 'x' << desc.Height
                << "-expected=" << expectedWidth << 'x' << expectedHeight;
        reason = message.str();
        return false;
    }
    if (allowedFormats.size() != 0
        && std::find(allowedFormats.begin(), allowedFormats.end(), desc.Format)
            == allowedFormats.end())
    {
        reason = std::string(label) + "-unsupported-format-"
            + std::to_string(static_cast<unsigned int>(desc.Format));
        return false;
    }
    return true;
}

const char* ToTraceQuality(const DlssQuality quality)
{
    switch (quality)
    {
    case DlssQuality::DLAA: return "dlaa";
    case DlssQuality::Quality: return "quality";
    case DlssQuality::Balanced: return "balanced";
    case DlssQuality::Performance: return "performance";
    case DlssQuality::UltraPerformance: return "ultra-performance";
    }
    return "unknown";
}

const char* ToTraceFeature(const DlssFrameInputs& inputs)
{
    return inputs.useRayReconstruction ? "rr" : "dlss";
}
} // namespace
#endif // GAME_ENGINE_ENABLE_DLSS

namespace
{
bool QuerySdkExtentRecommendation(
    void* const userData,
    const DlssExtentRecommendationKey& key,
    DlssExtentRecommendation& recommendation,
    std::string& failureReason)
{
    auto& context = *static_cast<DlssContext*>(userData);
    if (std::getenv("GAME_ENGINE_S2P2_FORCE_QUERY_FAILURE") != nullptr)
    {
        failureReason = "forced-query-failure";
        return false;
    }
#ifndef GAME_ENGINE_ENABLE_DLSS
    (void)context;
    (void)key;
    (void)recommendation;
    failureReason = "dlss-compiled-out";
    return false;
#else
    if (!context.IsReady() || !context.IsRuntimeInitialized())
    {
        failureReason = "runtime-unavailable";
        return false;
    }

    if (key.feature == DlssReconstructionFeature::RayReconstruction)
    {
        if (!context.IsRrSupported())
        {
            failureReason = "rr-feature-unavailable";
            return false;
        }
        if (g_slDLSSDGetOptimalSettings == nullptr)
        {
            failureReason = "rr-optimal-settings-entrypoint-unavailable";
            return false;
        }
        sl::DLSSDOptions options{};
        options.mode = ToDlssMode(key.quality);
        options.outputWidth = key.outputExtent.width;
        options.outputHeight = key.outputExtent.height;
        sl::DLSSDOptimalSettings settings{};
        const sl::Result result = g_slDLSSDGetOptimalSettings(options, settings);
        if (result != sl::Result::eOk)
        {
            failureReason = std::string("rr-optimal-settings-query-failed-") + ResultToString(result);
            return false;
        }
        recommendation = {
            {settings.optimalRenderWidth, settings.optimalRenderHeight},
            {settings.renderWidthMin, settings.renderHeightMin},
            {settings.renderWidthMax, settings.renderHeightMax}};
        return true;
    }

    if (!context.IsDlssSupported())
    {
        failureReason = "dlss-feature-unavailable";
        return false;
    }
    if (g_slDLSSGetOptimalSettings == nullptr)
    {
        failureReason = "dlss-optimal-settings-entrypoint-unavailable";
        return false;
    }
    sl::DLSSOptions options{};
    options.mode = ToDlssMode(key.quality);
    options.outputWidth = key.outputExtent.width;
    options.outputHeight = key.outputExtent.height;
    sl::DLSSOptimalSettings settings{};
    const sl::Result result = g_slDLSSGetOptimalSettings(options, settings);
    if (result != sl::Result::eOk)
    {
        failureReason = std::string("dlss-optimal-settings-query-failed-") + ResultToString(result);
        return false;
    }
    recommendation = {
        {settings.optimalRenderWidth, settings.optimalRenderHeight},
        {settings.renderWidthMin, settings.renderHeightMin},
        {settings.renderWidthMax, settings.renderHeightMax}};
    return true;
#endif
}
} // namespace

DlssPlannedExtent DlssContext::PlanReconstructionExtent(
    const DlssExtentRecommendationKey& key)
{
    std::lock_guard<std::mutex> lock(m_extentPlanMutex);
    DlssExtentPlanLookup lookup =
        m_extentPlanCache.Plan(key, &QuerySdkExtentRecommendation, this);

    // A viewport can request its first plan while asynchronous SDK initialization is still in
    // flight. Retry that one transient fallback once the runtime becomes queryable.
    if (lookup.cacheHit && lookup.plan.fallbackReason == "runtime-unavailable"
        && IsReady() && IsRuntimeInitialized())
    {
        m_extentPlanCache.Erase(key);
        lookup = m_extentPlanCache.Plan(key, &QuerySdkExtentRecommendation, this);
    }

    if (!lookup.cacheHit)
    {
        std::ostringstream message;
        message << "extent-plan viewport=" << key.viewportId
                << " feature=" << ExtentFeatureName(key.feature)
                << " quality=" << ExtentQualityName(key.quality)
                << " output=" << key.outputExtent.width << 'x' << key.outputExtent.height
                << " planned=" << lookup.plan.extent.recommended.width << 'x'
                << lookup.plan.extent.recommended.height
                << " range=" << lookup.plan.extent.minimum.width << 'x'
                << lookup.plan.extent.minimum.height << ".."
                << lookup.plan.extent.maximum.width << 'x'
                << lookup.plan.extent.maximum.height
                << " source=" << (lookup.plan.IsSdkRecommendation() ? "sdk" : "explicit-fallback")
                << " rr-no-arbitrary-drs=" << (lookup.plan.rrNoArbitraryDrs ? "true" : "false")
                << " active-allocation=s2-p4-plan-owned";
        if (lookup.plan.IsSdkRecommendation())
        {
            EngineLog::Info("dlss", message.str());
        }
        else
        {
            message << " fallback-reason=" << lookup.plan.fallbackReason;
            EngineLog::Warn("dlss", message.str());
        }
    }
    return lookup.plan;
}

void DlssContext::ClearPlannedExtentCache()
{
    std::lock_guard<std::mutex> lock(m_extentPlanMutex);
    m_extentPlanCache.Clear();
}

DlssFrameToken DlssFrameTokenState::BeginFrame(
    const AcquireFunction acquire,
    void* const userData)
{
    const std::uint32_t requestedFrameIndex = m_nextFrameIndex++;
    m_current = DlssFrameToken{nullptr, requestedFrameIndex};
    if (acquire == nullptr)
    {
        return m_current;
    }

    void* nativeToken = nullptr;
    std::uint32_t actualFrameIndex = requestedFrameIndex;
    if (acquire(userData, requestedFrameIndex, nativeToken, actualFrameIndex)
        && nativeToken != nullptr)
    {
        m_current = DlssFrameToken{nativeToken, actualFrameIndex};
    }
    return m_current;
}

void DlssContext::BeginFrame()
{
#ifndef GAME_ENGINE_ENABLE_DLSS
    m_frameTokenState.BeginFrame(nullptr, nullptr);
#else
    const auto acquire = [](void*, const std::uint32_t requestedFrameIndex,
                            void*& nativeToken, std::uint32_t& actualFrameIndex) -> bool
    {
        sl::FrameToken* token = nullptr;
        const std::uint32_t frameIndex = requestedFrameIndex;
        if (g_slGetNewFrameToken(token, &frameIndex) != sl::Result::eOk || token == nullptr)
        {
            return false;
        }
        nativeToken = token;
        actualFrameIndex = static_cast<std::uint32_t>(*token);
        return true;
    };
    const DlssFrameTokenState::AcquireFunction acquireFunction =
        IsRuntimeInitialized() && g_slGetNewFrameToken != nullptr ? +acquire : nullptr;
    m_frameTokenState.BeginFrame(acquireFunction, nullptr);
#endif
}

bool DlssContext::Evaluate(const DlssFrameToken& frameToken, const DlssFrameInputs& inputs)
{
#ifndef GAME_ENGINE_ENABLE_DLSS
    FrameDiagnostics::LogDlssEvent(inputs.viewportId, "dlss", "unknown", "skipped", "compiled-out", false, 0, false, 0);
    return false;
#else
    const auto logSkip = [&](const char* reason)
    {
        FrameDiagnostics::LogDlssEvent(
            inputs.viewportId, ToTraceFeature(inputs), ToTraceQuality(inputs.quality), "skipped", reason,
            false, 0, false, 0);
    };
    if (!IsReady() || !IsRuntimeInitialized())
    {
        logSkip("runtime-unavailable");
        return false;
    }
    if (inputs.useRayReconstruction)
    {
        if (!IsRrSupported() || g_slDLSSDSetOptions == nullptr || inputs.diffuseAlbedo == nullptr
            || inputs.specularAlbedo == nullptr || inputs.normalRoughness == nullptr)
        {
            logSkip("rr-unavailable-or-guides-missing");
            return false;
        }
    }
    else if (!IsDlssSupported() || g_slDLSSSetOptions == nullptr)
    {
        logSkip("dlss-unavailable");
        return false;
    }
    if (g_slGetNewFrameToken == nullptr || g_slSetConstants == nullptr
        || g_slSetTagForFrame == nullptr || g_slEvaluateFeature == nullptr)
    {
        logSkip("streamline-entrypoint-missing");
        return false;
    }
    if (inputs.commandList == nullptr || inputs.colorInput == nullptr
        || inputs.colorOutput == nullptr || inputs.depth == nullptr
        || inputs.motionVectors == nullptr)
    {
        logSkip("required-input-missing");
        return false;
    }

    auto* cmdList = static_cast<sl::CommandBuffer*>(inputs.commandList);
    const sl::ViewportHandle viewport(inputs.viewportId);

    if (!frameToken.IsValid())
    {
        FrameDiagnostics::LogDlssEvent(
            inputs.viewportId, ToTraceFeature(inputs), ToTraceQuality(inputs.quality), "failed",
            "application-frame-token-unavailable", false, 0, false, 0);
        return false;
    }

    DlssExtentRecommendationKey activeKey{};
    activeKey.viewportId = inputs.viewportId;
    activeKey.outputExtent = {inputs.displayWidth, inputs.displayHeight};
    activeKey.feature = inputs.useRayReconstruction
        ? DlssReconstructionFeature::RayReconstruction
        : DlssReconstructionFeature::SuperResolution;
    activeKey.quality = inputs.quality;
    std::string contractReason;
    const DlssExtent activeRenderExtent =
        ResolveDlssActiveRenderExtent(inputs.extentPlan, activeKey, contractReason);
    if (activeRenderExtent.width != inputs.renderWidth
        || activeRenderExtent.height != inputs.renderHeight)
    {
        if (contractReason.empty())
        {
            contractReason = "active-allocation-does-not-match-planned-extent";
        }
        EngineLog::Error("dlss", "S2-P4 extent contract failed: " + contractReason);
        FrameDiagnostics::LogDlssEvent(
            inputs.viewportId, ToTraceFeature(inputs), ToTraceQuality(inputs.quality), "failed",
            contractReason.c_str(), false, 0, false, 0);
        return false;
    }

    const auto validate = [&](void* const resource, const std::uint32_t width,
                              const std::uint32_t height,
                              const std::initializer_list<DXGI_FORMAT> formats,
                              const char* const label)
    {
        return ValidateTaggedTexture(resource, width, height, formats, label, contractReason);
    };
    bool resourcesValid =
        validate(inputs.colorInput, inputs.renderWidth, inputs.renderHeight, {}, "color-input")
        && validate(inputs.colorOutput, inputs.displayWidth, inputs.displayHeight, {}, "color-output")
        && validate(
            inputs.depth,
            inputs.renderWidth,
            inputs.renderHeight,
            {DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT,
             DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_D32_FLOAT},
            "depth")
        && validate(
            inputs.motionVectors,
            inputs.renderWidth,
            inputs.renderHeight,
            {DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32G32_FLOAT},
            "motion");
    if (resourcesValid && inputs.useRayReconstruction)
    {
        resourcesValid =
            validate(inputs.diffuseAlbedo, inputs.renderWidth, inputs.renderHeight, {}, "diffuse-albedo")
            && validate(inputs.specularAlbedo, inputs.renderWidth, inputs.renderHeight, {}, "specular-albedo")
            && validate(inputs.normalRoughness, inputs.renderWidth, inputs.renderHeight, {}, "normal-roughness")
            && (inputs.specularHitDistance == nullptr
                || validate(
                    inputs.specularHitDistance,
                    inputs.renderWidth,
                    inputs.renderHeight,
                    {DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R32_FLOAT},
                    "specular-hit-distance"));
    }
    const unsigned int states[] = {
        inputs.colorInputState, inputs.colorOutputState, inputs.depthState,
        inputs.motionVectorsState, inputs.diffuseAlbedoState, inputs.specularAlbedoState,
        inputs.normalRoughnessState, inputs.specularHitDistanceState};
    const std::size_t requiredStateCount = inputs.useRayReconstruction
        ? (inputs.specularHitDistance != nullptr ? 8u : 7u)
        : 4u;
    if (resourcesValid
        && std::find(std::begin(states), std::begin(states) + requiredStateCount, UINT_MAX)
            != std::begin(states) + requiredStateCount)
    {
        contractReason = "required-tag-state-is-unknown";
        resourcesValid = false;
    }
    if (!resourcesValid)
    {
        EngineLog::Error("dlss", "S2-P4 resource contract failed: " + contractReason);
        FrameDiagnostics::LogDlssEvent(
            inputs.viewportId, ToTraceFeature(inputs), ToTraceQuality(inputs.quality), "failed",
            contractReason.c_str(), false, 0, false, 0);
        return false;
    }
    auto* const nativeFrameToken = static_cast<sl::FrameToken*>(frameToken.native);
    const std::uint32_t frameIndex = frameToken.frameIndex;

    // Tags use the exact active allocation rectangles. All resources are per-viewport allocations
    // retained and unmodified until present; tracked states describe their state at this call.
    const sl::Extent inputExtent{0, 0, inputs.renderWidth, inputs.renderHeight};
    const sl::Extent outputExtent{0, 0, inputs.displayWidth, inputs.displayHeight};
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
    sl::Resource specularMotionVectors{};

    std::vector<sl::ResourceTag> tags;
    tags.reserve(8);
    tags.emplace_back(
        &depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &inputExtent);
    tags.emplace_back(
        &mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &inputExtent);
    tags.emplace_back(
        &colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &inputExtent);
    tags.emplace_back(
        &colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &outputExtent);
    if (inputs.useRayReconstruction)
    {
        // RR material guides (devdoc/dxr/dlss-rr.md). normalRoughness is PACKED (DLSSDNormalRoughnessMode::ePacked).
        diffuseAlbedo = MakeTex(
            inputs.diffuseAlbedo, inputs.diffuseAlbedoState, inputs.renderWidth, inputs.renderHeight);
        specularAlbedo = MakeTex(
            inputs.specularAlbedo, inputs.specularAlbedoState, inputs.renderWidth, inputs.renderHeight);
        normalRoughness = MakeTex(
            inputs.normalRoughness, inputs.normalRoughnessState, inputs.renderWidth, inputs.renderHeight);
        tags.emplace_back(
            &diffuseAlbedo, sl::kBufferTypeAlbedo, sl::ResourceLifecycle::eValidUntilPresent, &inputExtent);
        tags.emplace_back(
            &specularAlbedo, sl::kBufferTypeSpecularAlbedo, sl::ResourceLifecycle::eValidUntilPresent, &inputExtent);
        tags.emplace_back(
            &normalRoughness, sl::kBufferTypeNormalRoughness, sl::ResourceLifecycle::eValidUntilPresent, &inputExtent);
        // Optional spec hit-distance guide (RR4): present only when reflections ran this frame.
        if (inputs.specularHitDistance != nullptr)
        {
            specularHitDistance = MakeTex(
                inputs.specularHitDistance, inputs.specularHitDistanceState,
                inputs.renderWidth, inputs.renderHeight);
            tags.emplace_back(
                &specularHitDistance, sl::kBufferTypeSpecularHitDistance,
                sl::ResourceLifecycle::eValidUntilPresent, &inputExtent);
        }
        if (inputs.specularMotionVectors != nullptr)
        {
            specularMotionVectors = MakeTex(
                inputs.specularMotionVectors, inputs.specularMotionVectorsState,
                inputs.renderWidth, inputs.renderHeight);
            tags.emplace_back(
                &specularMotionVectors, sl::kBufferTypeSpecularMotionVectors,
                sl::ResourceLifecycle::eValidUntilPresent, &inputExtent);
        }
    }
    if (inputs.reset)
    {
        const auto motionDesc = static_cast<ID3D12Resource*>(inputs.motionVectors)->GetDesc();
        const auto depthDesc = static_cast<ID3D12Resource*>(inputs.depth)->GetDesc();
        std::ostringstream message;
        message << "active-contract viewport=" << inputs.viewportId
                << " feature=" << ToTraceFeature(inputs)
                << " quality=" << ToTraceQuality(inputs.quality)
                << " render=" << inputs.renderWidth << 'x' << inputs.renderHeight
                << " output=" << inputs.displayWidth << 'x' << inputs.displayHeight
                << " source=" << (inputs.extentPlan.IsSdkRecommendation() ? "sdk" : "explicit-fallback")
                << " motion-format=" << static_cast<unsigned int>(motionDesc.Format)
                << " depth-format=" << static_cast<unsigned int>(depthDesc.Format)
                << " states=" << inputs.colorInputState << ',' << inputs.colorOutputState << ','
                << inputs.depthState << ',' << inputs.motionVectorsState
                << " motion-scale=" << inputs.mvecScaleX << ',' << inputs.mvecScaleY
                << " tag-extents=explicit lifetimes=valid-until-present"
                << " rr-no-arbitrary-drs="
                << (inputs.extentPlan.rrNoArbitraryDrs ? "true" : "false");
        EngineLog::Info("dlss", message.str());
    }
    if (g_slSetTagForFrame(*nativeFrameToken, viewport, tags.data(), static_cast<uint32_t>(tags.size()), cmdList)
        != sl::Result::eOk)
    {
        FrameDiagnostics::LogDlssEvent(
            inputs.viewportId, ToTraceFeature(inputs), ToTraceQuality(inputs.quality), "failed",
            "tagging-failed", false, 0, true, frameIndex);
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
    consts.cameraMotionIncluded = inputs.cameraMotionIncluded ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    consts.orthographicProjection = sl::Boolean::eFalse;
    consts.motionVectors3D = sl::Boolean::eFalse;
    consts.motionVectorsDilated = inputs.motionVectorsDilated ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    consts.motionVectorsJittered = sl::Boolean::eFalse;
    consts.reset = inputs.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    if (g_slSetConstants(consts, *nativeFrameToken, viewport) != sl::Result::eOk)
    {
        FrameDiagnostics::LogDlssEvent(
            inputs.viewportId, ToTraceFeature(inputs), ToTraceQuality(inputs.quality), "failed",
            "constants-failed", false, 0, true, frameIndex);
        return false;
    }

    sl::Feature feature = sl::kFeatureDLSS;
    if (inputs.useRayReconstruction)
    {
        sl::DLSSDOptions rrOptions{};
        rrOptions.mode = ToDlssMode(inputs.quality);
        // D4: RR model preset. Set every per-mode field so the choice applies regardless of the
        // active quality mode; Streamline reloads the model internally when this changes.
        const sl::DLSSDPreset rrPreset =
            inputs.rrPreset == DlssRrPreset::TransformerD ? sl::DLSSDPreset::ePresetD
            : inputs.rrPreset == DlssRrPreset::TransformerE ? sl::DLSSDPreset::ePresetE
            : sl::DLSSDPreset::eDefault;
        rrOptions.dlaaPreset = rrPreset;
        rrOptions.qualityPreset = rrPreset;
        rrOptions.balancedPreset = rrPreset;
        rrOptions.performancePreset = rrPreset;
        rrOptions.ultraPerformancePreset = rrPreset;
        rrOptions.ultraQualityPreset = rrPreset;
        rrOptions.outputWidth = inputs.displayWidth;
        rrOptions.outputHeight = inputs.displayHeight;
        rrOptions.colorBuffersHDR = inputs.colorIsHdr ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        // S2-P1: the integrated RR programming contract does not define exposure guidance.
        // Leave the SDK defaults untouched; authored display EV is applied after reconstruction.
        rrOptions.sharpness = std::clamp(inputs.sharpness, 0.0f, 1.0f);
        rrOptions.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked;
        CopyMatrix(rrOptions.worldToCameraView, inputs.worldToCameraView);
        CopyMatrix(rrOptions.cameraViewToWorld, inputs.cameraViewToWorld);
        if (g_slDLSSDSetOptions(viewport, rrOptions) != sl::Result::eOk)
        {
            FrameDiagnostics::LogDlssEvent(
                inputs.viewportId, ToTraceFeature(inputs), ToTraceQuality(inputs.quality), "failed",
                "rr-options-failed", false, 0, true, frameIndex);
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
        // Ordinary DLSS accepts reconstruction guidance. These values are deliberately distinct
        // from authored display EV, which is applied after reconstruction by the renderer.
        options.preExposure = inputs.preExposure;
        options.exposureScale = inputs.exposureScale;
        options.sharpness = std::clamp(inputs.sharpness, 0.0f, 1.0f);
        if (g_slDLSSSetOptions(viewport, options) != sl::Result::eOk)
        {
            FrameDiagnostics::LogDlssEvent(
                inputs.viewportId, ToTraceFeature(inputs), ToTraceQuality(inputs.quality), "failed",
                "dlss-options-failed", false, 0, true, frameIndex);
            return false;
        }
    }

    const sl::BaseStructure* evalInputs[] = {&viewport};
    const sl::Result evalResult = g_slEvaluateFeature(
        feature, *nativeFrameToken, evalInputs, static_cast<uint32_t>(std::size(evalInputs)),
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
        FrameDiagnostics::LogDlssEvent(
            inputs.viewportId, ToTraceFeature(inputs), ToTraceQuality(inputs.quality), "failed",
            "evaluate-failed", false, 0, true, frameIndex);
        return false;
    }
    FrameDiagnostics::LogDlssEvent(
        inputs.viewportId, ToTraceFeature(inputs), ToTraceQuality(inputs.quality), "evaluated", "none",
        false, 0, true, frameIndex);
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
