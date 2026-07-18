#include "engine/rendering/post/ScreenSpaceEffectsSettings.h"

#include "engine/platform/diagnostics/EngineLog.h"
#include "engine/rhi/DlssContext.h"
#include "engine/rendering/post/ScreenSpaceEffects.h"

#include <nlohmann/json.hpp>

namespace ScreenSpaceEffectsSettings
{
namespace
{
    const char* AmbientOcclusionModeToString(const AmbientOcclusionMode mode)
    {
        switch (mode)
        {
        case AmbientOcclusionMode::SSAO:
            return "SSAO";
        case AmbientOcclusionMode::GTAO:
            return "GTAO";
        case AmbientOcclusionMode::Off:
        default:
            return "Off";
        }
    }

    AmbientOcclusionMode AmbientOcclusionModeFromString(const std::string& value)
    {
        if (value == "SSAO")
        {
            return AmbientOcclusionMode::SSAO;
        }
        if (value == "GTAO")
        {
            return AmbientOcclusionMode::GTAO;
        }

        return AmbientOcclusionMode::Off;
    }

    const char* TonemapModeToString(const TonemapMode mode)
    {
        switch (mode)
        {
        case TonemapMode::Gamma:
            return "Gamma";
        case TonemapMode::Reinhard:
            return "Reinhard";
        case TonemapMode::ACES:
            return "ACES";
        }

        return "Gamma";
    }

    TonemapMode TonemapModeFromString(const std::string& value)
    {
        if (value == "Reinhard")
        {
            return TonemapMode::Reinhard;
        }
        if (value == "ACES")
        {
            return TonemapMode::ACES;
        }

        return TonemapMode::Gamma;
    }

    const char* AntiAliasingModeToString(const AntiAliasingMode mode)
    {
        switch (mode)
        {
        case AntiAliasingMode::FXAA:
            return "FXAA";
        case AntiAliasingMode::TAA:
            return "TAA";
        case AntiAliasingMode::MSAA:
            return "MSAA";
        case AntiAliasingMode::SMAA:
            return "SMAA";
        case AntiAliasingMode::SSAA:
            return "SSAA";
        case AntiAliasingMode::DLAA:
            return "DLAA";
        case AntiAliasingMode::DLSS:
            return "DLSS";
        case AntiAliasingMode::None:
        default:
            return "None";
        }
    }

    AntiAliasingMode AntiAliasingModeFromString(const std::string& value)
    {
        if (value == "FXAA")
        {
            return AntiAliasingMode::FXAA;
        }
        if (value == "TAA")
        {
            return AntiAliasingMode::TAA;
        }
        if (value == "MSAA")
        {
            return AntiAliasingMode::MSAA;
        }
        if (value == "SMAA")
        {
            return AntiAliasingMode::SMAA;
        }
        if (value == "SSAA")
        {
            return AntiAliasingMode::SSAA;
        }
        if (value == "DLAA")
        {
            return AntiAliasingMode::DLAA;
        }
        if (value == "DLSS" || value == "dlss_sr")
        {
            return AntiAliasingMode::DLSS;
        }

        return AntiAliasingMode::None;
    }

    const char* DlssPresetToString(const DlssPreset preset)
    {
        switch (preset)
        {
        case DlssPreset::Balanced:
            return "balanced";
        case DlssPreset::Performance:
            return "performance";
        case DlssPreset::UltraPerformance:
            return "ultra_performance";
        case DlssPreset::Quality:
        default:
            return "quality";
        }
    }

    DlssPreset DlssPresetFromString(const std::string& value)
    {
        if (value == "balanced" || value == "Balanced")
        {
            return DlssPreset::Balanced;
        }
        if (value == "performance" || value == "Performance")
        {
            return DlssPreset::Performance;
        }
        if (value == "ultra_performance" || value == "UltraPerformance"
            || value == "Ultra Performance")
        {
            return DlssPreset::UltraPerformance;
        }

        return DlssPreset::Quality;
    }

    const char* DlssRrPresetToString(const DlssRrPreset preset)
    {
        switch (preset)
        {
        case DlssRrPreset::TransformerD:
            return "transformer_d";
        case DlssRrPreset::TransformerE:
            return "transformer_e";
        case DlssRrPreset::Default:
        default:
            return "default";
        }
    }

    DlssRrPreset DlssRrPresetFromString(const std::string& value)
    {
        if (value == "transformer_d" || value == "TransformerD")
        {
            return DlssRrPreset::TransformerD;
        }
        if (value == "transformer_e" || value == "TransformerE")
        {
            return DlssRrPreset::TransformerE;
        }

        return DlssRrPreset::Default;
    }

    bool AntiAliasingModeOwnsResolve(const AntiAliasingMode mode)
    {
        return mode == AntiAliasingMode::TAA || mode == AntiAliasingMode::DLAA
            || mode == AntiAliasingMode::DLSS;
    }

    bool IsDlssAntiAliasingMode(const AntiAliasingMode mode)
    {
        return mode == AntiAliasingMode::DLAA || mode == AntiAliasingMode::DLSS;
    }
} // namespace

nlohmann::json ToJson(const ScreenSpaceEffects& effects)
{
    const glm::vec4 ptGiDiagnosticRoi = effects.GetPathTracerGiDiagnosticRoi();
    return nlohmann::json{
        {"enabled", effects.IsEnabled()},
        {"ssaoEnabled", effects.IsSsaoEnabled()},
        {"aoMode", AmbientOcclusionModeToString(effects.GetAmbientOcclusionMode())},
        {"ssaoRadius", effects.GetSsaoRadius()},
        {"ssaoBias", effects.GetSsaoBias()},
        {"ssaoPower", effects.GetSsaoPower()},
        {"gtaoRadius", effects.GetGtaoRadius()},
        {"gtaoThickness", effects.GetGtaoThickness()},
        {"gtaoFalloff", effects.GetGtaoFalloff()},
        {"gtaoPower", effects.GetGtaoPower()},
        {"gtaoDirections", effects.GetGtaoDirections()},
        {"gtaoSteps", effects.GetGtaoSteps()},
        {"gtaoDenoiseEnabled", effects.IsGtaoDenoiseEnabled()},
        {"aoStrength", effects.GetAoStrength()},
        {"exposure", effects.GetExposure()},
        {"tonemapMode", TonemapModeToString(effects.GetTonemapMode())},
        {"bloomEnabled", effects.IsBloomEnabled()},
        {"bloomThreshold", effects.GetBloomThreshold()},
        {"bloomSoftKnee", effects.GetBloomSoftKnee()},
        {"bloomIntensity", effects.GetBloomIntensity()},
        {"bloomBlurRadius", effects.GetBloomBlurRadius()},
        {"antiAliasingMode", AntiAliasingModeToString(effects.GetAntiAliasingMode())},
        {"dlssPreset", DlssPresetToString(effects.GetDlssPreset())},
        {"dlssRayReconstruction", effects.GetRayReconstruction()},
        {"dlssSharpness", effects.GetDlssSharpness()},
        {"dlssRrPreset", DlssRrPresetToString(effects.GetRrPreset())},
        {"msaaSampleCount", effects.GetMsaaSampleCount()},
        {"fxaaSubpixQuality", effects.GetFxaaSubpixQuality()},
        {"fxaaEdgeThreshold", effects.GetFxaaEdgeThreshold()},
        {"renderScale", effects.GetRenderScale()},
        {"taaBlendFactor", effects.GetTaaBlendFactor()},
        {"giTemporalBlendFactor", effects.GetGiTemporalBlendFactor()},
        {"giDepthThreshold", effects.GetGiDepthThreshold()},
        {"ssgiDenoiseEnabled", effects.IsSsgiDenoiseEnabled()},
        {"ssgiNoiseInjectionEnabled", effects.IsSsgiNoiseInjectionEnabled()},
        {"ssgiNoiseStrength", effects.GetSsgiNoiseStrength()},
        {"ssgiSpatialBlurSpread", effects.GetSsgiSpatialBlurSpread()},
        {"ssgiSpatialDepthThreshold", effects.GetSsgiSpatialDepthThreshold()},
        {"ssgiRoughnessSpreadMin", effects.GetSsgiRoughnessSpreadMin()},
        {"ssgiRoughnessSpreadMax", effects.GetSsgiRoughnessSpreadMax()},
        {"ssgiEnabled", effects.IsSsgiEnabled()},
        {"ssgiStrength", effects.GetSsgiStrength()},
        {"ssgiMaxTraceDistance", effects.GetSsgiMaxTraceDistance()},
        {"ssgiStepCount", effects.GetSsgiStepCount()},
        {"ssgiThickness", effects.GetSsgiThickness()},
        {"ssrEnabled", effects.IsSsrEnabled()},
        {"ssrMaxTraceDistance", effects.GetSsrMaxTraceDistance()},
        {"ssrStepCount", effects.GetSsrStepCount()},
        {"ssrSampleCount", effects.GetSsrSampleCount()},
        {"ssrThickness", effects.GetSsrThickness()},
        {"ssrRoughnessCutoff", effects.GetSsrRoughnessCutoff()},
        {"ssrStrength", effects.GetSsrStrength()},
        {"ssrDenoiseEnabled", effects.IsSsrDenoiseEnabled()},
        {"ssrTemporalBlendFactor", effects.GetSsrTemporalBlendFactor()},
        {"smaaThreshold", effects.GetSmaaThreshold()},
        {"smaaSearchSteps", effects.GetSmaaSearchSteps()},
        {"ssaoBlurDepthThreshold", effects.GetSsaoBlurDepthThreshold()},
        {"ptGiDiagnosticRoi", nlohmann::json::array({
            ptGiDiagnosticRoi.x,
            ptGiDiagnosticRoi.y,
            ptGiDiagnosticRoi.z,
            ptGiDiagnosticRoi.w})},
    };
}

void ApplyFromJson(ScreenSpaceEffects& effects, const nlohmann::json& effectsValue)
{
    if (effectsValue.contains("ptGiDiagnosticRoi")
        && effectsValue.at("ptGiDiagnosticRoi").is_array()
        && effectsValue.at("ptGiDiagnosticRoi").size() == 4)
    {
        const nlohmann::json& roi = effectsValue.at("ptGiDiagnosticRoi");
        effects.SetPathTracerGiDiagnosticRoi(glm::vec4(
            roi[0].get<float>(),
            roi[1].get<float>(),
            roi[2].get<float>(),
            roi[3].get<float>()));
    }
    if (effectsValue.contains("enabled"))
    {
        effects.SetEnabled(effectsValue.at("enabled").get<bool>());
    }
    if (effectsValue.contains("aoMode"))
    {
        effects.SetAmbientOcclusionMode(
            AmbientOcclusionModeFromString(effectsValue.at("aoMode").get<std::string>()));
    }
    else if (effectsValue.contains("ssaoEnabled"))
    {
        effects.SetSsaoEnabled(effectsValue.at("ssaoEnabled").get<bool>());
    }
    if (effectsValue.contains("ssaoRadius"))
    {
        effects.SetSsaoRadius(effectsValue.at("ssaoRadius").get<float>());
    }
    if (effectsValue.contains("ssaoBias"))
    {
        effects.SetSsaoBias(effectsValue.at("ssaoBias").get<float>());
    }
    if (effectsValue.contains("ssaoPower"))
    {
        effects.SetSsaoPower(effectsValue.at("ssaoPower").get<float>());
    }
    if (effectsValue.contains("gtaoRadius"))
    {
        effects.SetGtaoRadius(effectsValue.at("gtaoRadius").get<float>());
    }
    if (effectsValue.contains("gtaoThickness"))
    {
        effects.SetGtaoThickness(effectsValue.at("gtaoThickness").get<float>());
    }
    if (effectsValue.contains("gtaoFalloff"))
    {
        effects.SetGtaoFalloff(effectsValue.at("gtaoFalloff").get<float>());
    }
    if (effectsValue.contains("gtaoPower"))
    {
        effects.SetGtaoPower(effectsValue.at("gtaoPower").get<float>());
    }
    if (effectsValue.contains("gtaoDirections"))
    {
        effects.SetGtaoDirections(effectsValue.at("gtaoDirections").get<int>());
    }
    if (effectsValue.contains("gtaoSteps"))
    {
        effects.SetGtaoSteps(effectsValue.at("gtaoSteps").get<int>());
    }
    if (effectsValue.contains("gtaoDenoiseEnabled"))
    {
        effects.SetGtaoDenoiseEnabled(effectsValue.at("gtaoDenoiseEnabled").get<bool>());
    }
    if (effectsValue.contains("aoStrength"))
    {
        effects.SetAoStrength(effectsValue.at("aoStrength").get<float>());
    }
    if (effectsValue.contains("exposure"))
    {
        effects.SetExposure(effectsValue.at("exposure").get<float>());
    }
    if (effectsValue.contains("tonemapMode"))
    {
        effects.SetTonemapMode(
            TonemapModeFromString(effectsValue.at("tonemapMode").get<std::string>()));
    }
    if (effectsValue.contains("bloomEnabled"))
    {
        effects.SetBloomEnabled(effectsValue.at("bloomEnabled").get<bool>());
    }
    if (effectsValue.contains("bloomThreshold"))
    {
        effects.SetBloomThreshold(effectsValue.at("bloomThreshold").get<float>());
    }
    if (effectsValue.contains("bloomSoftKnee"))
    {
        effects.SetBloomSoftKnee(effectsValue.at("bloomSoftKnee").get<float>());
    }
    if (effectsValue.contains("bloomIntensity"))
    {
        effects.SetBloomIntensity(effectsValue.at("bloomIntensity").get<float>());
    }
    if (effectsValue.contains("bloomBlurRadius"))
    {
        effects.SetBloomBlurRadius(effectsValue.at("bloomBlurRadius").get<float>());
    }
    if (effectsValue.contains("dlssPreset"))
    {
        effects.SetDlssPreset(
            DlssPresetFromString(effectsValue.at("dlssPreset").get<std::string>()));
    }
    if (effectsValue.contains("dlssRayReconstruction"))
    {
        effects.SetRayReconstruction(effectsValue.at("dlssRayReconstruction").get<bool>());
    }
    if (effectsValue.contains("dlssSharpness"))
    {
        effects.SetDlssSharpness(effectsValue.at("dlssSharpness").get<float>());
    }
    if (effectsValue.contains("dlssRrPreset"))
    {
        effects.SetRrPreset(
            DlssRrPresetFromString(effectsValue.at("dlssRrPreset").get<std::string>()));
    }
    if (effectsValue.contains("fxaaSubpixQuality"))
    {
        effects.SetFxaaSubpixQuality(effectsValue.at("fxaaSubpixQuality").get<float>());
    }
    if (effectsValue.contains("fxaaEdgeThreshold"))
    {
        effects.SetFxaaEdgeThreshold(effectsValue.at("fxaaEdgeThreshold").get<float>());
    }
    if (effectsValue.contains("renderScale"))
    {
        effects.SetRenderScale(effectsValue.at("renderScale").get<float>());
    }
    if (effectsValue.contains("taaBlendFactor"))
    {
        effects.SetTaaBlendFactor(effectsValue.at("taaBlendFactor").get<float>());
    }
    if (effectsValue.contains("giTemporalBlendFactor"))
    {
        effects.SetGiTemporalBlendFactor(effectsValue.at("giTemporalBlendFactor").get<float>());
    }
    if (effectsValue.contains("giDepthThreshold"))
    {
        effects.SetGiDepthThreshold(effectsValue.at("giDepthThreshold").get<float>());
    }
    if (effectsValue.contains("ssgiDenoiseEnabled"))
    {
        effects.SetSsgiDenoiseEnabled(effectsValue.at("ssgiDenoiseEnabled").get<bool>());
    }
    if (effectsValue.contains("ssgiNoiseInjectionEnabled"))
    {
        effects.SetSsgiNoiseInjectionEnabled(effectsValue.at("ssgiNoiseInjectionEnabled").get<bool>());
    }
    if (effectsValue.contains("ssgiNoiseStrength"))
    {
        effects.SetSsgiNoiseStrength(effectsValue.at("ssgiNoiseStrength").get<float>());
    }
    if (effectsValue.contains("ssgiSpatialBlurSpread"))
    {
        effects.SetSsgiSpatialBlurSpread(effectsValue.at("ssgiSpatialBlurSpread").get<float>());
    }
    if (effectsValue.contains("ssgiSpatialDepthThreshold"))
    {
        effects.SetSsgiSpatialDepthThreshold(effectsValue.at("ssgiSpatialDepthThreshold").get<float>());
    }
    if (effectsValue.contains("ssgiRoughnessSpreadMin"))
    {
        effects.SetSsgiRoughnessSpreadMin(effectsValue.at("ssgiRoughnessSpreadMin").get<float>());
    }
    if (effectsValue.contains("ssgiRoughnessSpreadMax"))
    {
        effects.SetSsgiRoughnessSpreadMax(effectsValue.at("ssgiRoughnessSpreadMax").get<float>());
    }
    if (effectsValue.contains("ssgiEnabled"))
    {
        effects.SetSsgiEnabled(effectsValue.at("ssgiEnabled").get<bool>());
    }
    if (effectsValue.contains("ssgiStrength"))
    {
        effects.SetSsgiStrength(effectsValue.at("ssgiStrength").get<float>());
    }
    if (effectsValue.contains("ssgiMaxTraceDistance"))
    {
        effects.SetSsgiMaxTraceDistance(effectsValue.at("ssgiMaxTraceDistance").get<float>());
    }
    if (effectsValue.contains("ssgiStepCount"))
    {
        effects.SetSsgiStepCount(effectsValue.at("ssgiStepCount").get<int>());
    }
    if (effectsValue.contains("ssgiThickness"))
    {
        effects.SetSsgiThickness(effectsValue.at("ssgiThickness").get<float>());
    }
    if (effectsValue.contains("ssrEnabled"))
    {
        effects.SetSsrEnabled(effectsValue.at("ssrEnabled").get<bool>());
    }
    if (effectsValue.contains("ssrMaxTraceDistance"))
    {
        effects.SetSsrMaxTraceDistance(effectsValue.at("ssrMaxTraceDistance").get<float>());
    }
    if (effectsValue.contains("ssrStepCount"))
    {
        effects.SetSsrStepCount(effectsValue.at("ssrStepCount").get<int>());
    }
    if (effectsValue.contains("ssrSampleCount"))
    {
        effects.SetSsrSampleCount(effectsValue.at("ssrSampleCount").get<int>());
    }
    if (effectsValue.contains("ssrThickness"))
    {
        effects.SetSsrThickness(effectsValue.at("ssrThickness").get<float>());
    }
    if (effectsValue.contains("ssrRoughnessCutoff"))
    {
        effects.SetSsrRoughnessCutoff(effectsValue.at("ssrRoughnessCutoff").get<float>());
    }
    if (effectsValue.contains("ssrStrength"))
    {
        effects.SetSsrStrength(effectsValue.at("ssrStrength").get<float>());
    }
    if (effectsValue.contains("ssrDenoiseEnabled"))
    {
        effects.SetSsrDenoiseEnabled(effectsValue.at("ssrDenoiseEnabled").get<bool>());
    }
    if (effectsValue.contains("ssrTemporalBlendFactor"))
    {
        effects.SetSsrTemporalBlendFactor(effectsValue.at("ssrTemporalBlendFactor").get<float>());
    }
    if (effectsValue.contains("smaaThreshold"))
    {
        effects.SetSmaaThreshold(effectsValue.at("smaaThreshold").get<float>());
    }
    if (effectsValue.contains("smaaSearchSteps"))
    {
        effects.SetSmaaSearchSteps(effectsValue.at("smaaSearchSteps").get<int>());
    }
    if (effectsValue.contains("ssaoBlurDepthThreshold"))
    {
        effects.SetSsaoBlurDepthThreshold(effectsValue.at("ssaoBlurDepthThreshold").get<float>());
    }
}

LoadedAntiAliasingSettings NormalizeAntiAliasingSettings(
    AntiAliasingMode antiAliasingMode,
    const int msaaSampleCount)
{
    LoadedAntiAliasingSettings settings{};
    settings.antiAliasingMode = antiAliasingMode;
    settings.msaaSampleCount = msaaSampleCount;

    if (settings.antiAliasingMode == AntiAliasingMode::MSAA)
    {
        if (settings.msaaSampleCount <= 1)
        {
            settings.msaaSampleCount = 4;
        }
        settings.antiAliasingMode = AntiAliasingMode::None;
    }
    if (settings.msaaSampleCount > 1 && settings.antiAliasingMode == AntiAliasingMode::TAA)
    {
        settings.antiAliasingMode = AntiAliasingMode::None;
    }
    if (settings.antiAliasingMode == AntiAliasingMode::TAA && settings.msaaSampleCount > 1)
    {
        settings.msaaSampleCount = 1;
    }
    if (AntiAliasingModeOwnsResolve(settings.antiAliasingMode) && settings.msaaSampleCount > 1)
    {
        settings.msaaSampleCount = 1;
    }
    if (settings.msaaSampleCount > 1 && AntiAliasingModeOwnsResolve(settings.antiAliasingMode))
    {
        settings.antiAliasingMode = AntiAliasingMode::None;
    }
    if (IsDlssAntiAliasingMode(settings.antiAliasingMode)
        && DlssContext::Get().IsReady() && !DlssContext::Get().IsDlssSupported())
    {
        EngineLog::Warn(
            "dlss",
            "Project requests DLSS but this GPU/driver does not support it; falling back to TAA.");
        settings.antiAliasingMode = AntiAliasingMode::TAA;
    }

    return settings;
}

LoadedAntiAliasingSettings ResolveLoadedAntiAliasingSettings(const nlohmann::json& rendererValue)
{
    LoadedAntiAliasingSettings settings{};
    if (!rendererValue.contains("screenSpaceEffects"))
    {
        return settings;
    }

    const nlohmann::json& effectsValue = rendererValue.at("screenSpaceEffects");
    const AntiAliasingMode loadedAaMode = AntiAliasingModeFromString(effectsValue.value(
        "antiAliasingMode",
        AntiAliasingModeToString(AntiAliasingMode::None)));
    const int loadedMsaaSampleCount = effectsValue.value("msaaSampleCount", 1);
    return NormalizeAntiAliasingSettings(loadedAaMode, loadedMsaaSampleCount);
}

LoadedAntiAliasingSettings ResolveAntiAliasingDelta(
    const nlohmann::json& effectsValue,
    AntiAliasingMode currentMode,
    const int currentMsaaSampleCount)
{
    AntiAliasingMode aaMode = currentMode;
    int msaaSampleCount = currentMsaaSampleCount;
    if (effectsValue.contains("antiAliasingMode"))
    {
        aaMode = AntiAliasingModeFromString(effectsValue.at("antiAliasingMode").get<std::string>());
    }
    if (effectsValue.contains("msaaSampleCount"))
    {
        msaaSampleCount = effectsValue.at("msaaSampleCount").get<int>();
    }

    return NormalizeAntiAliasingSettings(aaMode, msaaSampleCount);
}

} // namespace ScreenSpaceEffectsSettings
