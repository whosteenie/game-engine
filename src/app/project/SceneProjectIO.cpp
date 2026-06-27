#include "app/project/SceneProjectIO.h"

#include "app/project/ProjectEditorState.h"
#include "app/project/SceneImportedMeshPool.h"
#include "engine/platform/NativeProgressWindow.h"

#include "app/scene/Scene.h"
#include "app/scene/SceneMeshLibrary.h"
#include "app/scene/SceneObjectStore.h"
#include "app/scene/SceneRenderer.h"
#include "app/scene/SceneSpawnService.h"
#include "engine/rendering/Constants.h"
#include "engine/lighting/EnvironmentMap.h"
#include "engine/lighting/IBL.h"
#include "engine/components/CameraComponent.h"
#include "engine/components/ColliderComponent.h"
#include "engine/scene/InspectorComponentOrder.h"
#include "engine/components/ComponentSerialization.h"
#include "engine/scene/InspectorComponentOrderJson.h"
#include "engine/scene/JsonMath.h"
#include "engine/components/LightComponent.h"
#include "engine/components/RigidBodyComponent.h"
#include "engine/rendering/Material.h"
#include "engine/rendering/Mesh.h"
#include "engine/rendering/ShaderCache.h"
#include "engine/assets/ModelImporter.h"
#include "engine/platform/EngineLog.h"
#include "engine/platform/ExceptionMessage.h"
#include "engine/scene/SceneObject.h"
#include "engine/scene/SceneObjectId.h"
#include "engine/scene/ScenePrimitive.h"
#include "engine/lighting/DirectionalShadowSettings.h"
#include "engine/rendering/ScreenSpaceEffects.h"
#include "engine/rendering/TextureSamplerSettings.h"
#include "engine/rendering/Texture.h"
#include "engine/assets/TextureCache.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <unordered_map>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace SceneProjectIODetail
{
    constexpr const char* kFormatId = "game-engine-project";
    constexpr float kProjectObjectLoadProgressStart = 0.15f;
    constexpr float kProjectObjectLoadProgressEnd = 0.75f;

    std::string NormalizeSlashes(std::string path)
    {
        for (char& character : path)
        {
            if (character == '\\')
            {
                character = '/';
            }
        }

        return path;
    }

    std::string ToProjectRelativePath(const std::string& projectRoot, const std::string& path)
    {
        if (path.empty())
        {
            return {};
        }

        const std::string normalized = NormalizeSlashes(path);
        const fs::path stored(path);

        if (!stored.is_absolute() && normalized.rfind("..", 0) != 0 && !projectRoot.empty())
        {
            const fs::path rooted = fs::path(projectRoot) / stored;
            std::error_code existsError;
            if (fs::exists(rooted, existsError))
            {
                return NormalizeSlashes(stored.generic_string());
            }
        }

        std::error_code error;
        const fs::path absolutePath = fs::weakly_canonical(fs::path(path), error);
        if (error)
        {
            return NormalizeSlashes(path);
        }

        if (!projectRoot.empty())
        {
            const fs::path rootPath = fs::weakly_canonical(fs::path(projectRoot), error);
            if (!error)
            {
                std::error_code relativeError;
                const fs::path relativePath = fs::relative(absolutePath, rootPath, relativeError);
                if (!relativeError && !relativePath.empty() && relativePath.generic_string().rfind("..", 0) != 0)
                {
                    return NormalizeSlashes(relativePath.generic_string());
                }
            }
        }

        return NormalizeSlashes(absolutePath.generic_string());
    }

    std::string ResolveProjectPath(const std::string& projectRoot, const std::string& storedPath)
    {
        return SceneProjectIO::ResolveProjectPath(projectRoot, storedPath);
    }

    std::optional<ScenePrimitive> DetectPrimitiveMesh(const Scene& scene, Mesh* mesh)
    {
        if (mesh == nullptr)
        {
            return std::nullopt;
        }

        const ScenePrimitive primitives[] = {
            ScenePrimitive::Cube,
            ScenePrimitive::Sphere,
            ScenePrimitive::Cylinder,
            ScenePrimitive::Capsule,
            ScenePrimitive::Plane,
        };

        for (ScenePrimitive primitive : primitives)
        {
            if (mesh == scene.GetMeshLibrary().GetPrimitive(primitive))
            {
                return primitive;
            }
        }

        return std::nullopt;
    }

    bool TryGetPrimitiveLocalBounds(ScenePrimitive primitive, glm::vec3& outMin, glm::vec3& outMax)
    {
        switch (primitive)
        {
        case ScenePrimitive::Cube:
        case ScenePrimitive::Sphere:
        case ScenePrimitive::Cylinder:
            outMin = glm::vec3(-0.5f);
            outMax = glm::vec3(0.5f);
            return true;
        case ScenePrimitive::Capsule:
            outMin = glm::vec3(-0.5f, -1.0f, -0.5f);
            outMax = glm::vec3(0.5f, 1.0f, 0.5f);
            return true;
        case ScenePrimitive::Plane:
            outMin = glm::vec3(-Scene::FloorHalfExtent, -0.01f, -Scene::FloorHalfExtent);
            outMax = glm::vec3(Scene::FloorHalfExtent, 0.01f, Scene::FloorHalfExtent);
            return true;
        }

        return false;
    }

    bool TryGetMeshLocalBounds(const Scene& scene, Mesh* mesh, glm::vec3& outMin, glm::vec3& outMax)
    {
        const std::optional<ScenePrimitive> primitive = DetectPrimitiveMesh(scene, mesh);
        return primitive.has_value() && TryGetPrimitiveLocalBounds(*primitive, outMin, outMax);
    }

    json SerializeMesh(const Scene& scene, const SceneObject& object, const std::string& projectRoot)
    {
        if (!object.HasMesh())
        {
            return json{{"kind", "none"}};
        }

        if (const std::optional<ScenePrimitive> primitive = DetectPrimitiveMesh(scene, object.GetMesh()))
        {
            return json{
                {"kind", "primitive"},
                {"primitive", ScenePrimitiveToString(*primitive)},
            };
        }

        if (!object.GetImportAssetPath().empty() && object.GetImportNodeIndex() >= 0)
        {
            return json{
                {"kind", "imported"},
                {"assetPath", ToProjectRelativePath(projectRoot, object.GetImportAssetPath())},
                {"nodeIndex", object.GetImportNodeIndex()},
            };
        }

        return json{{"kind", "none"}};
    }

    struct ImportedAssetCacheEntry
    {
        ImportedModel model;
        bool loaded = false;
        std::string errorMessage;
    };

    Mesh* AcquireImportedMesh(
        Scene& scene,
        const std::string& projectRoot,
        const std::string& storedAssetPath,
        int nodeIndex,
        std::unordered_map<std::string, ImportedAssetCacheEntry>& importCache,
        ImportedMeshReusePool* meshReusePool,
        std::string& outError)
    {
        const std::string resolvedPath = ResolveProjectPath(projectRoot, storedAssetPath);
        if (meshReusePool != nullptr)
        {
            const ImportMeshKey reuseKey{resolvedPath, nodeIndex};
            const auto reuseIterator = meshReusePool->find(reuseKey);
            if (reuseIterator != meshReusePool->end())
            {
                Mesh* reusedMesh = scene.GetMeshLibrary().AdoptImportedMesh(std::move(reuseIterator->second));
                meshReusePool->erase(reuseIterator);
                return reusedMesh;
            }
        }

        ImportedAssetCacheEntry& cacheEntry = importCache[resolvedPath];
        if (!cacheEntry.loaded)
        {
            cacheEntry.model = LoadModelFromFile(
                resolvedPath,
                projectRoot,
                {},
                ModelLoadMode::GeometryOnly);
            cacheEntry.loaded = true;
            cacheEntry.errorMessage = cacheEntry.model.errorMessage;
        }

        if (!cacheEntry.errorMessage.empty())
        {
            outError = cacheEntry.errorMessage;
            return nullptr;
        }

        if (nodeIndex < 0 || nodeIndex >= static_cast<int>(cacheEntry.model.nodes.size()))
        {
            outError = "Imported model node index is out of range: " + resolvedPath;
            return nullptr;
        }

        ImportedSceneNode& node = cacheEntry.model.nodes[static_cast<std::size_t>(nodeIndex)];
        if (!node.hasMesh || node.mesh == nullptr)
        {
            outError = "Imported model node does not contain mesh data: " + resolvedPath;
            return nullptr;
        }

        return scene.GetMeshLibrary().AdoptImportedMesh(std::move(node.mesh));
    }

    bool DeserializeObjectMesh(
        Scene& scene,
        const json& meshValue,
        const std::string& projectRoot,
        std::unordered_map<std::string, ImportedAssetCacheEntry>& importCache,
        ImportedMeshReusePool* meshReusePool,
        Mesh*& outMesh,
        std::string& outImportAssetPath,
        int& outImportNodeIndex,
        std::string& outError)
    {
        outMesh = nullptr;
        outImportAssetPath.clear();
        outImportNodeIndex = -1;

        const std::string kind = meshValue.value("kind", "none");
        if (kind == "none")
        {
            return true;
        }

        if (kind == "primitive")
        {
            ScenePrimitive primitive = ScenePrimitive::Cube;
            if (!TryParseScenePrimitive(meshValue.at("primitive").get<std::string>().c_str(), primitive))
            {
                outError = "Unknown primitive mesh type.";
                return false;
            }

            outMesh = scene.GetMeshLibrary().GetPrimitive(primitive);
            return true;
        }

        if (kind == "imported")
        {
            const std::string assetPath = meshValue.at("assetPath").get<std::string>();
            const int nodeIndex = meshValue.at("nodeIndex").get<int>();
            outMesh = AcquireImportedMesh(
                scene,
                projectRoot,
                assetPath,
                nodeIndex,
                importCache,
                meshReusePool,
                outError);
            if (outMesh == nullptr)
            {
                return false;
            }

            outImportAssetPath = ResolveProjectPath(projectRoot, assetPath);
            outImportNodeIndex = nodeIndex;
            return true;
        }

        outError = "Unknown mesh kind in project file.";
        return false;
    }

    json SerializeBoolMap(const std::unordered_map<std::string, bool>& values)
    {
        json result = json::object();
        for (const auto& [key, value] : values)
        {
            result[key] = value;
        }
        return result;
    }

    std::unordered_map<std::string, bool> DeserializeBoolMap(const json& value)
    {
        std::unordered_map<std::string, bool> result;
        if (!value.is_object())
        {
            return result;
        }

        for (const auto& [key, entry] : value.items())
        {
            if (entry.is_boolean())
            {
                result[key] = entry.get<bool>();
            }
        }

        return result;
    }

    json SerializeHierarchyOpenStates(const std::unordered_map<SceneObjectId, bool>& openStates)
    {
        json result = json::object();
        for (const auto& [objectId, isOpen] : openStates)
        {
            if (isOpen)
            {
                result[std::to_string(objectId)] = true;
            }
        }
        return result;
    }

    std::unordered_map<SceneObjectId, bool> DeserializeHierarchyOpenStates(const json& value)
    {
        std::unordered_map<SceneObjectId, bool> result;
        if (!value.is_object())
        {
            return result;
        }

        for (const auto& [key, entry] : value.items())
        {
            if (!entry.is_boolean() || !entry.get<bool>())
            {
                continue;
            }

            try
            {
                result[std::stoull(key)] = true;
            }
            catch (const std::exception&)
            {
            }
        }

        return result;
    }

    const char* TonemapModeToString(TonemapMode mode)
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

    const char* AntiAliasingModeToString(AntiAliasingMode mode)
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

        return AntiAliasingMode::None;
    }

    struct LoadedScreenSpaceAaSettings
    {
        AntiAliasingMode antiAliasingMode = AntiAliasingMode::None;
        int msaaSampleCount = 1;
    };

    LoadedScreenSpaceAaSettings ResolveLoadedScreenSpaceAaSettings(const json& rendererValue)
    {
        LoadedScreenSpaceAaSettings settings{};
        if (!rendererValue.contains("screenSpaceEffects"))
        {
            return settings;
        }

        const json& effectsValue = rendererValue.at("screenSpaceEffects");
        AntiAliasingMode loadedAaMode = AntiAliasingModeFromString(effectsValue.value(
            "antiAliasingMode",
            AntiAliasingModeToString(AntiAliasingMode::None)));
        int loadedMsaaSampleCount = effectsValue.value("msaaSampleCount", 1);
        if (loadedAaMode == AntiAliasingMode::MSAA)
        {
            if (loadedMsaaSampleCount <= 1)
            {
                loadedMsaaSampleCount = 4;
            }
            loadedAaMode = AntiAliasingMode::None;
        }
        if (loadedMsaaSampleCount > 1 && loadedAaMode == AntiAliasingMode::TAA)
        {
            loadedAaMode = AntiAliasingMode::None;
        }
        if (loadedAaMode == AntiAliasingMode::TAA && loadedMsaaSampleCount > 1)
        {
            loadedMsaaSampleCount = 1;
        }

        settings.antiAliasingMode = loadedAaMode;
        settings.msaaSampleCount = loadedMsaaSampleCount;
        return settings;
    }

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

    const char* TextureFilterModeToString(TextureFilterMode mode)
    {
        switch (mode)
        {
        case TextureFilterMode::Bilinear:
            return "Bilinear";
        case TextureFilterMode::Nearest:
            return "Nearest";
        case TextureFilterMode::Trilinear:
        default:
            return "Trilinear";
        }
    }

    TextureFilterMode TextureFilterModeFromString(const std::string& value)
    {
        if (value == "Bilinear")
        {
            return TextureFilterMode::Bilinear;
        }
        if (value == "Nearest")
        {
            return TextureFilterMode::Nearest;
        }

        return TextureFilterMode::Trilinear;
    }

    const char* ShadowFilterModeToString(DirectionalShadowFilterMode mode)
    {
        return mode == DirectionalShadowFilterMode::PCSS ? "PCSS" : "PCF";
    }

    DirectionalShadowFilterMode ShadowFilterModeFromString(const std::string& value)
    {
        if (value == "PCSS")
        {
            return DirectionalShadowFilterMode::PCSS;
        }

        return DirectionalShadowFilterMode::PCF;
    }

    json SerializeRenderer(const Scene& scene)
    {
        const ScreenSpaceEffects& effects = scene.GetRenderer().GetScreenSpaceEffects();
        const DirectionalShadowSettings& shadowSettings = scene.GetRenderer().GetDirectionalShadowSettings();
        return json{
            {"environmentIntensity", scene.GetRenderer().GetIBL().GetEnvironmentIntensity()},
            {"skybox",
             json{
                 {"enabled", scene.GetRenderer().GetEnvironmentMap().IsEnabled()},
                 {"backgroundMode", static_cast<int>(scene.GetRenderer().GetEnvironmentMap().GetBackgroundMode())},
                 {"hdrPath", scene.GetRenderer().GetEnvironmentMap().GetHdrPath()},
                 {"rotationDegrees", scene.GetRenderer().GetEnvironmentMap().GetRotationDegrees()},
                 {"exposure", scene.GetRenderer().GetEnvironmentMap().GetExposure()},
                 {"iblCubemapResolution",
                  static_cast<int>(scene.GetRenderer().GetEnvironmentMap().GetIblCubemapResolution())},
                 {"solidBackgroundColor",
                  json::array(
                      {scene.GetRenderer().GetEnvironmentMap().GetSolidBackgroundColorSrgb().x,
                       scene.GetRenderer().GetEnvironmentMap().GetSolidBackgroundColorSrgb().y,
                       scene.GetRenderer().GetEnvironmentMap().GetSolidBackgroundColorSrgb().z})},
             }},
            {"textureFilterMode", TextureFilterModeToString(scene.GetRenderer().GetTextureFilterMode())},
            {"textureAnisotropy", scene.GetRenderer().GetTextureAnisotropy()},
            {"textureMipBias", scene.GetRenderer().GetTextureMipBias()},
            {"directionalShadow",
             json{
                 {"filterMode", ShadowFilterModeToString(shadowSettings.GetFilterMode())},
                 {"shadowMapResolution", shadowSettings.GetShadowMapResolution()},
                 {"cascadeCount", shadowSettings.GetCascadeCount()},
                 {"cascadeSplitLambda", shadowSettings.GetCascadeSplitLambda()},
                 {"cascadeBlendRatio", shadowSettings.GetCascadeBlendRatio()},
                 {"tightNearPlaneXyFit", shadowSettings.GetTightNearPlaneXyFit()},
                 {"xyMarginFraction", shadowSettings.GetXyMarginFraction()},
                 {"zMarginFraction", shadowSettings.GetZMarginFraction()},
                 {"usePoissonPcf", shadowSettings.GetUsePoissonPcf()},
                 {"pcfKernelRadius", shadowSettings.GetPcfKernelRadius()},
                 {"pcfSampleCount", shadowSettings.GetPcfSampleCount()},
                 {"minPenumbraTexels", shadowSettings.GetMinPenumbraTexels()},
                 {"sunAngularDiameterDegrees", shadowSettings.GetSunAngularDiameterDegrees()},
                 {"pcssLightAngularSize", shadowSettings.GetPcssLightAngularSize()},
                 {"pcssBlockerRadius", shadowSettings.GetPcssBlockerRadius()},
                 {"pcssMinPenumbraTexels", shadowSettings.GetPcssMinPenumbraTexels()},
                 {"pcssMaxPenumbraTexels", shadowSettings.GetPcssMaxPenumbraTexels()},
                 {"worldBiasScale", shadowSettings.GetWorldBiasScale()},
                 {"depthBiasScale", shadowSettings.GetDepthBiasScale()},
                 {"casterDepthBiasScale", shadowSettings.GetCasterDepthBiasScale()},
                 {"shadowBlurEnabled", shadowSettings.GetShadowBlurEnabled()},
                 {"shadowBlurRadius", shadowSettings.GetShadowBlurRadius()},
                 {"shadowBlurDepthThreshold", shadowSettings.GetShadowBlurDepthThreshold()},
                 {"shadowBlurShadowThreshold", shadowSettings.GetShadowBlurShadowThreshold()},
             }},
            {"screenSpaceEffects",
             json{
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
                 {"smaaThreshold", effects.GetSmaaThreshold()},
                 {"smaaSearchSteps", effects.GetSmaaSearchSteps()},
                 {"ssaoBlurDepthThreshold", effects.GetSsaoBlurDepthThreshold()},
             }},
        };
    }

    void DeserializeRenderer(Scene& scene, const json& rendererValue)
    {
        SceneRenderer& renderer = scene.GetRenderer();
        const LoadedScreenSpaceAaSettings loadedAaSettings =
            ResolveLoadedScreenSpaceAaSettings(rendererValue);
        if (loadedAaSettings.msaaSampleCount > 1)
        {
            ShaderCache::Clear();
        }
        renderer.PrepareGpuResourcesForGeometryMsaa(loadedAaSettings.msaaSampleCount);
        const bool gpuReady = renderer.IsGpuResourcesReady();
        if (gpuReady && loadedAaSettings.msaaSampleCount > 1)
        {
            scene.InvalidateAllMaterialCachedShaders();
        }

        if (gpuReady)
        {
            const float defaultEnvironmentIntensity = renderer.GetIBL().GetEnvironmentIntensity();
            renderer.GetIBL().SetEnvironmentIntensity(
                rendererValue.value("environmentIntensity", defaultEnvironmentIntensity));
            EnvironmentMap& environmentMap = renderer.GetEnvironmentMap();
            if (rendererValue.contains("skybox"))
            {
                const json& skyboxValue = rendererValue.at("skybox");
                environmentMap.SetEnabled(skyboxValue.value("enabled", environmentMap.IsEnabled()));
                environmentMap.SetBackgroundMode(static_cast<EnvironmentBackgroundMode>(
                    skyboxValue.value(
                        "backgroundMode",
                        static_cast<int>(environmentMap.GetBackgroundMode()))));
                environmentMap.SetHdrPath(skyboxValue.value("hdrPath", environmentMap.GetHdrPath()));
                environmentMap.SetRotationDegrees(
                    skyboxValue.value("rotationDegrees", environmentMap.GetRotationDegrees()));
                environmentMap.SetExposure(skyboxValue.value("exposure", environmentMap.GetExposure()));
                environmentMap.SetIblCubemapResolution(static_cast<EnvironmentIblCubemapResolution>(
                    skyboxValue.value(
                        "iblCubemapResolution",
                        static_cast<int>(environmentMap.GetIblCubemapResolution()))));
                if (skyboxValue.contains("solidBackgroundColor")
                    && skyboxValue.at("solidBackgroundColor").is_array()
                    && skyboxValue.at("solidBackgroundColor").size() == 3)
                {
                    const json& colorValue = skyboxValue.at("solidBackgroundColor");
                    environmentMap.SetSolidBackgroundColorSrgb(glm::vec3(
                        colorValue[0].get<float>(),
                        colorValue[1].get<float>(),
                        colorValue[2].get<float>()));
                }
            }
            renderer.SetTextureFilterMode(TextureFilterModeFromString(rendererValue.value(
                "textureFilterMode",
                TextureFilterModeToString(renderer.GetTextureFilterMode()))));
            renderer.SetTextureAnisotropy(rendererValue.value(
                "textureAnisotropy",
                renderer.GetTextureAnisotropy()));
            renderer.SetTextureMipBias(rendererValue.value(
                "textureMipBias",
                renderer.GetTextureMipBias()));
        }

        if (rendererValue.contains("directionalShadow"))
        {
            const json& shadowValue = rendererValue.at("directionalShadow");
            DirectionalShadowSettings& shadowSettings = renderer.GetDirectionalShadowSettings();
            shadowSettings.SetFilterMode(ShadowFilterModeFromString(
                shadowValue.value("filterMode", ShadowFilterModeToString(shadowSettings.GetFilterMode()))));
            shadowSettings.SetShadowMapResolution(
                shadowValue.value("shadowMapResolution", shadowSettings.GetShadowMapResolution()));
            shadowSettings.SetCascadeCount(shadowValue.value("cascadeCount", shadowSettings.GetCascadeCount()));
            shadowSettings.SetCascadeSplitLambda(
                shadowValue.value("cascadeSplitLambda", shadowSettings.GetCascadeSplitLambda()));
            shadowSettings.SetCascadeBlendRatio(
                shadowValue.value("cascadeBlendRatio", shadowSettings.GetCascadeBlendRatio()));
            shadowSettings.SetTightNearPlaneXyFit(
                shadowValue.value("tightNearPlaneXyFit", shadowSettings.GetTightNearPlaneXyFit()));
            shadowSettings.SetXyMarginFraction(
                shadowValue.value("xyMarginFraction", shadowSettings.GetXyMarginFraction()));
            shadowSettings.SetZMarginFraction(
                shadowValue.value("zMarginFraction", shadowSettings.GetZMarginFraction()));
            shadowSettings.SetUsePoissonPcf(
                shadowValue.value("usePoissonPcf", shadowSettings.GetUsePoissonPcf()));
            shadowSettings.SetPcfKernelRadius(
                shadowValue.value("pcfKernelRadius", shadowSettings.GetPcfKernelRadius()));
            shadowSettings.SetPcfSampleCount(
                shadowValue.value("pcfSampleCount", shadowSettings.GetPcfSampleCount()));
            shadowSettings.SetMinPenumbraTexels(
                shadowValue.value("minPenumbraTexels", shadowSettings.GetMinPenumbraTexels()));
            shadowSettings.SetSunAngularDiameterDegrees(shadowValue.value(
                "sunAngularDiameterDegrees",
                shadowSettings.GetSunAngularDiameterDegrees()));
            shadowSettings.SetPcssLightAngularSize(
                shadowValue.value("pcssLightAngularSize", shadowSettings.GetPcssLightAngularSize()));
            shadowSettings.SetPcssBlockerRadius(
                shadowValue.value("pcssBlockerRadius", shadowSettings.GetPcssBlockerRadius()));
            shadowSettings.SetPcssMinPenumbraTexels(
                shadowValue.value("pcssMinPenumbraTexels", shadowSettings.GetPcssMinPenumbraTexels()));
            shadowSettings.SetPcssMaxPenumbraTexels(
                shadowValue.value("pcssMaxPenumbraTexels", shadowSettings.GetPcssMaxPenumbraTexels()));
            shadowSettings.SetWorldBiasScale(
                shadowValue.value("worldBiasScale", shadowSettings.GetWorldBiasScale()));
            shadowSettings.SetDepthBiasScale(
                shadowValue.value("depthBiasScale", shadowSettings.GetDepthBiasScale()));
            shadowSettings.SetCasterDepthBiasScale(
                shadowValue.value("casterDepthBiasScale", shadowSettings.GetCasterDepthBiasScale()));
            shadowSettings.SetShadowBlurEnabled(
                shadowValue.value("shadowBlurEnabled", shadowSettings.GetShadowBlurEnabled()));
            shadowSettings.SetShadowBlurRadius(
                shadowValue.value("shadowBlurRadius", shadowSettings.GetShadowBlurRadius()));
            shadowSettings.SetShadowBlurDepthThreshold(shadowValue.value(
                "shadowBlurDepthThreshold",
                shadowSettings.GetShadowBlurDepthThreshold()));
            shadowSettings.SetShadowBlurShadowThreshold(shadowValue.value(
                "shadowBlurShadowThreshold",
                shadowSettings.GetShadowBlurShadowThreshold()));
        }

        if (!gpuReady || !rendererValue.contains("screenSpaceEffects"))
        {
            return;
        }

        const json& effectsValue = rendererValue.at("screenSpaceEffects");
        ScreenSpaceEffects& effects = renderer.GetScreenSpaceEffects();
        effects.SetEnabled(effectsValue.value("enabled", effects.IsEnabled()));
        if (effectsValue.contains("aoMode"))
        {
            effects.SetAmbientOcclusionMode(AmbientOcclusionModeFromString(effectsValue.value(
                "aoMode",
                AmbientOcclusionModeToString(effects.GetAmbientOcclusionMode()))));
        }
        else
        {
            effects.SetSsaoEnabled(effectsValue.value("ssaoEnabled", effects.IsSsaoEnabled()));
        }
        effects.SetSsaoRadius(effectsValue.value("ssaoRadius", effects.GetSsaoRadius()));
        effects.SetSsaoBias(effectsValue.value("ssaoBias", effects.GetSsaoBias()));
        effects.SetSsaoPower(effectsValue.value("ssaoPower", effects.GetSsaoPower()));
        effects.SetGtaoRadius(effectsValue.value("gtaoRadius", effects.GetGtaoRadius()));
        effects.SetGtaoThickness(effectsValue.value("gtaoThickness", effects.GetGtaoThickness()));
        effects.SetGtaoFalloff(effectsValue.value("gtaoFalloff", effects.GetGtaoFalloff()));
        effects.SetGtaoPower(effectsValue.value("gtaoPower", effects.GetGtaoPower()));
        effects.SetGtaoDirections(effectsValue.value("gtaoDirections", effects.GetGtaoDirections()));
        effects.SetGtaoSteps(effectsValue.value("gtaoSteps", effects.GetGtaoSteps()));
        effects.SetGtaoDenoiseEnabled(effectsValue.value(
            "gtaoDenoiseEnabled",
            effects.IsGtaoDenoiseEnabled()));
        effects.SetAoStrength(effectsValue.value("aoStrength", effects.GetAoStrength()));
        effects.SetExposure(effectsValue.value("exposure", effects.GetExposure()));
        effects.SetTonemapMode(
            TonemapModeFromString(effectsValue.value("tonemapMode", TonemapModeToString(effects.GetTonemapMode()))));
        effects.SetBloomEnabled(effectsValue.value("bloomEnabled", effects.IsBloomEnabled()));
        effects.SetBloomThreshold(effectsValue.value("bloomThreshold", effects.GetBloomThreshold()));
        effects.SetBloomSoftKnee(effectsValue.value("bloomSoftKnee", effects.GetBloomSoftKnee()));
        effects.SetBloomIntensity(effectsValue.value("bloomIntensity", effects.GetBloomIntensity()));
        effects.SetBloomBlurRadius(effectsValue.value("bloomBlurRadius", effects.GetBloomBlurRadius()));
        effects.SetAntiAliasingMode(loadedAaSettings.antiAliasingMode);
        effects.SetMsaaSampleCount(loadedAaSettings.msaaSampleCount);
        effects.SetFxaaSubpixQuality(
            effectsValue.value("fxaaSubpixQuality", effects.GetFxaaSubpixQuality()));
        effects.SetFxaaEdgeThreshold(
            effectsValue.value("fxaaEdgeThreshold", effects.GetFxaaEdgeThreshold()));
        effects.SetRenderScale(effectsValue.value("renderScale", effects.GetRenderScale()));
        effects.SetTaaBlendFactor(effectsValue.value("taaBlendFactor", effects.GetTaaBlendFactor()));
        effects.SetGiTemporalBlendFactor(
            effectsValue.value("giTemporalBlendFactor", effects.GetGiTemporalBlendFactor()));
        effects.SetGiDepthThreshold(effectsValue.value("giDepthThreshold", effects.GetGiDepthThreshold()));
        effects.SetSsgiDenoiseEnabled(
            effectsValue.value("ssgiDenoiseEnabled", effects.IsSsgiDenoiseEnabled()));
        effects.SetSsgiNoiseInjectionEnabled(effectsValue.value(
            "ssgiNoiseInjectionEnabled",
            effects.IsSsgiNoiseInjectionEnabled()));
        effects.SetSsgiNoiseStrength(
            effectsValue.value("ssgiNoiseStrength", effects.GetSsgiNoiseStrength()));
        effects.SetSsgiSpatialBlurSpread(
            effectsValue.value("ssgiSpatialBlurSpread", effects.GetSsgiSpatialBlurSpread()));
        effects.SetSsgiSpatialDepthThreshold(effectsValue.value(
            "ssgiSpatialDepthThreshold",
            effects.GetSsgiSpatialDepthThreshold()));
        effects.SetSsgiRoughnessSpreadMin(effectsValue.value(
            "ssgiRoughnessSpreadMin",
            effects.GetSsgiRoughnessSpreadMin()));
        effects.SetSsgiRoughnessSpreadMax(effectsValue.value(
            "ssgiRoughnessSpreadMax",
            effects.GetSsgiRoughnessSpreadMax()));
        effects.SetSsgiEnabled(effectsValue.value("ssgiEnabled", effects.IsSsgiEnabled()));
        effects.SetSsgiStrength(effectsValue.value("ssgiStrength", effects.GetSsgiStrength()));
        effects.SetSsgiMaxTraceDistance(effectsValue.value(
            "ssgiMaxTraceDistance",
            effects.GetSsgiMaxTraceDistance()));
        effects.SetSsgiStepCount(effectsValue.value("ssgiStepCount", effects.GetSsgiStepCount()));
        effects.SetSsgiThickness(effectsValue.value("ssgiThickness", effects.GetSsgiThickness()));
        effects.SetSmaaThreshold(effectsValue.value("smaaThreshold", effects.GetSmaaThreshold()));
        effects.SetSmaaSearchSteps(effectsValue.value("smaaSearchSteps", effects.GetSmaaSearchSteps()));
        effects.SetSsaoBlurDepthThreshold(
            effectsValue.value("ssaoBlurDepthThreshold", effects.GetSsaoBlurDepthThreshold()));
    }

    json SerializeProjectFilesFolderOpenStates(
        const std::unordered_map<std::string, bool>& openStates,
        const std::string& projectRoot)
    {
        json result = json::object();
        for (const auto& [path, isOpen] : openStates)
        {
            if (!isOpen)
            {
                continue;
            }

            const std::string storedPath = ToProjectRelativePath(projectRoot, path);
            if (!storedPath.empty())
            {
                result[storedPath] = true;
            }
        }
        return result;
    }

    std::unordered_map<std::string, bool> DeserializeProjectFilesFolderOpenStates(
        const json& value,
        const std::string& projectRoot)
    {
        std::unordered_map<std::string, bool> result;
        if (!value.is_object())
        {
            return result;
        }

        for (const auto& [key, entry] : value.items())
        {
            if (!entry.is_boolean() || !entry.get<bool>())
            {
                continue;
            }

            const std::string resolvedPath = ResolveProjectPath(projectRoot, key);
            if (!resolvedPath.empty())
            {
                result[resolvedPath] = true;
            }
        }

        return result;
    }

    json SerializeEditorState(const Scene& scene, const ProjectEditorState& editorState, const std::string& projectRoot)
    {
        return json{
            {"showGrid", scene.GetShowGrid()},
            {"showLightGizmos", scene.GetShowLightGizmos()},
            {"camera",
             json{
                 {"position", Vec3ToJson(editorState.cameraPosition)},
                 {"yaw", editorState.cameraYaw},
                 {"pitch", editorState.cameraPitch},
             }},
            {"panels",
             json{
                 {"hierarchy", editorState.showHierarchy},
                 {"inspector", editorState.showInspector},
                 {"toolbar", editorState.showToolbar},
                 {"lighting", editorState.showLighting},
                 {"performance", editorState.showPerformance},
                 {"projectFiles", editorState.showProjectFiles},
                 {"sceneView", editorState.showSceneView},
                 {"gameView", editorState.showGameView},
             }},
            {"hierarchyOpenNodes", SerializeHierarchyOpenStates(editorState.hierarchyNodeOpenStates)},
            {"projectFiles",
             json{
                 {"browsedDirectory",
                  ToProjectRelativePath(projectRoot, editorState.projectFilesBrowsedDirectory)},
                 {"selectedPath", ToProjectRelativePath(projectRoot, editorState.projectFilesSelectedPath)},
                 {"folderOpenStates", SerializeProjectFilesFolderOpenStates(
                      editorState.projectFilesFolderOpenStates, projectRoot)},
             }},
        };
    }

    void DeserializeEditorState(
        const json& editorValue,
        ProjectEditorState& editorState,
        const std::string& projectRoot)
    {
        editorState = ProjectEditorState::CreateDefault();

        if (editorValue.contains("camera"))
        {
            const json& cameraValue = editorValue.at("camera");
            editorState.cameraPosition = Vec3FromJson(cameraValue.at("position"));
            editorState.cameraYaw = cameraValue.value("yaw", editorState.cameraYaw);
            editorState.cameraPitch = cameraValue.value("pitch", editorState.cameraPitch);
        }

        if (editorValue.contains("panels"))
        {
            const json& panelsValue = editorValue.at("panels");
            editorState.showHierarchy = panelsValue.value("hierarchy", editorState.showHierarchy);
            editorState.showInspector = panelsValue.value("inspector", editorState.showInspector);
            editorState.showToolbar = panelsValue.value("toolbar", editorState.showToolbar);
            editorState.showLighting = panelsValue.value("lighting", editorState.showLighting);
            editorState.showPerformance = panelsValue.value("performance", editorState.showPerformance);
            editorState.showProjectFiles = panelsValue.value("projectFiles", editorState.showProjectFiles);
            editorState.showSceneView = panelsValue.value("sceneView", editorState.showSceneView);
            editorState.showGameView = panelsValue.value("gameView", editorState.showGameView);
        }

        if (editorValue.contains("hierarchyOpenNodes"))
        {
            editorState.hierarchyNodeOpenStates =
                DeserializeHierarchyOpenStates(editorValue.at("hierarchyOpenNodes"));
        }

        if (editorValue.contains("projectFiles"))
        {
            const json& projectFilesValue = editorValue.at("projectFiles");
            const std::string browsedDirectory = projectFilesValue.value("browsedDirectory", std::string{});
            const std::string selectedPath = projectFilesValue.value("selectedPath", std::string{});
            editorState.projectFilesBrowsedDirectory = ResolveProjectPath(projectRoot, browsedDirectory);
            editorState.projectFilesSelectedPath = ResolveProjectPath(projectRoot, selectedPath);
            if (projectFilesValue.contains("folderOpenStates"))
            {
                editorState.projectFilesFolderOpenStates =
                    DeserializeProjectFilesFolderOpenStates(projectFilesValue.at("folderOpenStates"), projectRoot);
            }
        }
    }

    json SerializeObjects(const Scene& scene, const std::string& projectRoot)
    {
        json objects = json::array();
        for (const SceneObject& object : scene.GetObjects())
        {
            json entry = json{
                {"id", object.GetId()},
                {"name", object.GetName()},
                {"parentIndex", object.GetParentIndex()},
                {"siblingOrder", object.GetSiblingOrder()},
                {"transform",
                 json{
                     {"position", Vec3ToJson(object.GetTransform().position)},
                     {"rotation", QuatToJson(object.GetTransform().rotation)},
                     {"scale", Vec3ToJson(object.GetTransform().scale)},
                 }},
                {"bounds",
                 json{
                     {"min", Vec3ToJson(object.GetLocalBoundsMin())},
                     {"max", Vec3ToJson(object.GetLocalBoundsMax())},
                 }},
                {"castShadow", object.CastsShadow()},
                {"receiveShadow", object.ReceivesShadow()},
                {"mesh", SerializeMesh(scene, object, projectRoot)},
            };

            if (object.HasMaterial())
            {
                entry["material"] = MaterialToJson(
                    object.GetMaterial(),
                    [&](const std::string& path) { return ToProjectRelativePath(projectRoot, path); });
            }

            if (object.HasLight())
            {
                entry["light"] = LightComponentToJson(object.GetLight());
            }

            if (object.HasCamera())
            {
                entry["camera"] = CameraComponentToJson(object.GetCamera());
            }

            if (object.HasRigidBody())
            {
                entry["rigidBody"] = RigidBodyComponentToJson(object.GetRigidBody());
            }

            if (object.HasCollider())
            {
                entry["collider"] = ColliderComponentToJson(object.GetCollider());
            }

            if (!object.GetInspectorComponentOrder().empty())
            {
                entry["inspectorComponentOrder"] = InspectorComponentOrderToJson(object);
            }

            objects.push_back(std::move(entry));
        }

        return objects;
    }

    json SerializeSpawnCounters(const Scene& scene)
    {
        const SceneSpawnCounters counters = scene.GetSpawnService().GetCounters();
        return json{
            {"directionalLight", counters.directionalLight},
            {"pointLight", counters.pointLight},
            {"spotLight", counters.spotLight},
            {"cube", counters.cube},
            {"sphere", counters.sphere},
            {"cylinder", counters.cylinder},
            {"capsule", counters.capsule},
            {"plane", counters.plane},
            {"empty", counters.empty},
            {"camera", counters.camera},
            {"import", counters.import},
        };
    }

    void DeserializeSpawnCounters(Scene& scene, const json& counters)
    {
        SceneSpawnCounters values = scene.GetSpawnService().GetCounters();
        values.directionalLight = counters.value("directionalLight", values.directionalLight);
        values.pointLight = counters.value("pointLight", values.pointLight);
        values.spotLight = counters.value("spotLight", values.spotLight);
        values.cube = counters.value("cube", values.cube);
        values.sphere = counters.value("sphere", values.sphere);
        values.cylinder = counters.value("cylinder", values.cylinder);
        values.capsule = counters.value("capsule", values.capsule);
        values.plane = counters.value("plane", values.plane);
        values.empty = counters.value("empty", values.empty);
        values.camera = counters.value("camera", values.camera);
        values.import = counters.value("import", values.import);
        scene.GetSpawnService().SetCounters(values);
    }

    json SerializeSelection(const Scene& scene)
    {
        json ids = json::array();
        for (SceneObjectId id : scene.GetSelectionIds())
        {
            ids.push_back(id);
        }

        SceneObjectId primaryId = kInvalidSceneObjectId;
        const int primaryIndex = scene.GetPrimarySelection();
        if (primaryIndex >= 0 && primaryIndex < static_cast<int>(scene.GetObjects().size()))
        {
            primaryId = scene.GetObjects()[static_cast<std::size_t>(primaryIndex)].GetId();
        }

        return json{
            {"ids", std::move(ids)},
            {"primary", primaryId},
        };
    }

    void DeserializeSelection(Scene& scene, const json& selection)
    {
        std::vector<SceneObjectId> ids;
        if (selection.contains("ids") && selection.at("ids").is_array())
        {
            for (const json& idValue : selection.at("ids"))
            {
                ids.push_back(idValue.get<SceneObjectId>());
            }
        }

        const SceneObjectId primary = selection.value("primary", kInvalidSceneObjectId);
        scene.SetSelectionByIds(ids, primary);
    }

    bool DeserializeObjects(
        Scene& scene,
        const json& objectValues,
        int formatVersion,
        const std::string& projectRoot,
        std::string& outError,
        ImportedMeshReusePool* meshReusePool,
        bool showProgress)
    {
        std::unordered_map<std::string, ImportedAssetCacheEntry> importCache;
        std::vector<SceneObject>& sceneObjects = scene.GetObjects();
        sceneObjects.reserve(objectValues.size());

        try
        {
            scene.GetMeshLibrary().GetPrimitive(ScenePrimitive::Cube);
        }
        catch (const std::exception& exception)
        {
            outError = FormatExceptionContext("Failed to create primitive meshes before loading objects", exception);
            EngineLog::LogFailure("project-io", "DeserializeObjects", outError);
            return false;
        }
        catch (...)
        {
            outError = "Failed to create primitive meshes before loading objects: unknown exception";
            EngineLog::LogFailure("project-io", "DeserializeObjects", outError);
            return false;
        }

        const std::size_t objectCount = objectValues.size();
        std::unique_ptr<ScopedNativeProgress> loadProgress;
        if (showProgress && objectCount > 0)
        {
            loadProgress = std::make_unique<ScopedNativeProgress>(
                "Loading Project",
                "Loading scene objects...");
            loadProgress->SetProgress(kProjectObjectLoadProgressStart);
        }

        std::size_t objectIndex = 0;
        for (const json& objectValue : objectValues)
        {
            ++objectIndex;
            if (loadProgress != nullptr)
            {
                const std::string objectName = objectValue.value("name", "object");
                loadProgress->SetMessage(
                    "Loading '" + objectName + "' (" + std::to_string(objectIndex) + "/" +
                    std::to_string(objectCount) + ")");
                const float objectProgress =
                    static_cast<float>(objectIndex) / static_cast<float>(objectCount);
                loadProgress->SetProgress(
                    kProjectObjectLoadProgressStart
                    + objectProgress * (kProjectObjectLoadProgressEnd - kProjectObjectLoadProgressStart));
            }

            try
            {
            Mesh* mesh = nullptr;
            std::string importAssetPath;
            int importNodeIndex = -1;
            if (!DeserializeObjectMesh(
                    scene,
                    objectValue.at("mesh"),
                    projectRoot,
                    importCache,
                    meshReusePool,
                    mesh,
                    importAssetPath,
                    importNodeIndex,
                    outError))
            {
                return false;
            }

            std::unique_ptr<Material> material;
            if (objectValue.contains("material"))
            {
                material = MaterialFromJson(
                    objectValue.at("material"),
                    [&](const std::string& storedPath) { return ResolveProjectPath(projectRoot, storedPath); },
                    [&](const std::string& path) { return ToProjectRelativePath(projectRoot, path); });
            }

            std::optional<LightComponent> light;
            if (objectValue.contains("light"))
            {
                light = LightComponentFromJson(objectValue.at("light"));
            }

            std::optional<CameraComponent> camera;
            if (objectValue.contains("camera"))
            {
                camera = CameraComponentFromJson(objectValue.at("camera"));
            }

            std::optional<RigidBodyComponent> rigidBody;
            if (objectValue.contains("rigidBody"))
            {
                rigidBody = RigidBodyComponentFromJson(objectValue.at("rigidBody"));
            }

            std::optional<ColliderComponent> collider;
            if (objectValue.contains("collider"))
            {
                collider = ColliderComponentFromJson(objectValue.at("collider"));
            }

            Transform transform;
            const json& transformValue = objectValue.at("transform");
            transform.position = Vec3FromJson(transformValue.at("position"));
            transform.rotation = QuatFromJson(transformValue.at("rotation"));
            transform.scale = Vec3FromJson(transformValue.at("scale"));

            glm::vec3 boundsMin(0.0f);
            glm::vec3 boundsMax(0.0f);
            if (objectValue.contains("bounds"))
            {
                const json& boundsValue = objectValue.at("bounds");
                boundsMin = Vec3FromJson(boundsValue.at("min"));
                boundsMax = Vec3FromJson(boundsValue.at("max"));
            }
            else if (!TryGetMeshLocalBounds(scene, mesh, boundsMin, boundsMax))
            {
                boundsMin = glm::vec3(-0.5f);
                boundsMax = glm::vec3(0.5f);
            }

            SceneObjectId objectId = kInvalidSceneObjectId;
            if (formatVersion >= 3 && objectValue.contains("id"))
            {
                objectId = objectValue.at("id").get<SceneObjectId>();
            }

            sceneObjects.emplace_back(
                objectValue.at("name").get<std::string>(),
                mesh,
                std::move(material),
                boundsMin,
                boundsMax,
                transform,
                objectValue.value("castShadow", true),
                objectValue.value("receiveShadow", true),
                objectValue.at("parentIndex").get<int>(),
                objectValue.at("siblingOrder").get<int>(),
                std::move(light),
                std::move(camera),
                std::move(rigidBody),
                std::move(collider),
                objectId);

            SceneObject& createdObject = sceneObjects.back();
            if (!importAssetPath.empty())
            {
                createdObject.SetImportSource(importAssetPath, importNodeIndex);
            }

            if (objectValue.contains("inspectorComponentOrder"))
            {
                createdObject.SetInspectorComponentOrder(
                    InspectorComponentOrderFromJson(objectValue.at("inspectorComponentOrder")));
            }

            if (formatVersion >= 3 && objectId != kInvalidSceneObjectId)
            {
                scene.GetObjectStore().RegisterId(objectId);
            }
            else
            {
                scene.GetObjectStore().FinalizeNewObject(createdObject);
            }
            }
            catch (const std::exception& exception)
            {
                const std::string context = "Failed loading object '" + objectValue.value("name", "?") + "'";
                outError = FormatExceptionContext(context.c_str(), exception);
                EngineLog::LogException("project-io", "DeserializeObjects", exception);
                EngineLog::LogFailure("project-io", "DeserializeObjects", outError);
                return false;
            }
            catch (...)
            {
                const std::string context = "Failed loading object '" + objectValue.value("name", "?") + "'";
                outError = context + ": unknown exception";
                EngineLog::LogFailure("project-io", "DeserializeObjects", outError);
                return false;
            }
        }

        int mainCameraIndex = -1;
        for (std::size_t index = 0; index < sceneObjects.size(); ++index)
        {
            SceneObject& object = sceneObjects[index];
            if (!object.HasCamera() || !object.GetCamera().isMain)
            {
                continue;
            }

            if (mainCameraIndex >= 0)
            {
                object.GetCamera().isMain = false;
            }
            else
            {
                mainCameraIndex = static_cast<int>(index);
            }
        }

        return true;
    }

    void EnsureNextObjectId(Scene& scene)
    {
        SceneObjectId maxId = 0;
        for (const SceneObject& object : scene.GetObjects())
        {
            maxId = std::max(maxId, object.GetId());
        }

        if (scene.GetObjectStore().GetNextId() <= maxId)
        {
            scene.GetObjectStore().SetNextId(maxId + 1);
        }
    }

    json SerializeSceneContent(const Scene& scene, const std::string& projectRoot)
    {
        return json{
            {"objects", SerializeObjects(scene, projectRoot)},
            {"spawnCounters", SerializeSpawnCounters(scene)},
            {"nextObjectId", scene.GetObjectStore().GetNextId()},
            {"selection", SerializeSelection(scene)},
        };
    }

    bool DeserializeSceneContent(
        Scene& scene,
        const json& content,
        int formatVersion,
        const std::string& projectRoot,
        std::string& outError,
        ImportedMeshReusePool* meshReusePool,
        bool showProgress)
    {
        if (!content.contains("objects") || !content.at("objects").is_array())
        {
            outError = "Scene content is missing objects.";
            return false;
        }

        ImportedMeshReusePool harvestedMeshes;
        if (meshReusePool == nullptr)
        {
            scene.GetMeshLibrary().HarvestImportedMeshes(scene.GetObjects(), harvestedMeshes);
            meshReusePool = &harvestedMeshes;
        }

        scene.GetObjectStore().Clear();
        scene.GetMeshLibrary().ClearImportedMeshes();

        if (!DeserializeObjects(
                scene,
                content.at("objects"),
                formatVersion,
                projectRoot,
                outError,
                meshReusePool,
                showProgress))
        {
            scene.GetObjectStore().Clear();
            scene.GetMeshLibrary().ClearImportedMeshes();
            scene.ClearSelection();
            return false;
        }

        if (content.contains("spawnCounters"))
        {
            DeserializeSpawnCounters(scene, content.at("spawnCounters"));
        }

        if (content.contains("nextObjectId"))
        {
            scene.GetObjectStore().SetNextId(content.at("nextObjectId").get<SceneObjectId>());
        }
        else
        {
            EnsureNextObjectId(scene);
        }

        if (content.contains("selection"))
        {
            DeserializeSelection(scene, content.at("selection"));
        }
        else
        {
            scene.ClearSelection();
        }

        return true;
    }
}

json SceneProjectIO::SerializeScene(
    const Scene& scene,
    const ProjectEditorState& editorState,
    const std::string& projectRoot)
    {
        return json{
            {"format", SceneProjectIODetail::kFormatId},
            {"version", SceneProjectIO::CurrentFormatVersion},
            {"scene",
             json{
                 {"objects", SceneProjectIODetail::SerializeObjects(scene, projectRoot)},
                 {"editor", SceneProjectIODetail::SerializeEditorState(scene, editorState, projectRoot)},
                 {"renderer", SceneProjectIODetail::SerializeRenderer(scene)},
                 {"spawnCounters", SceneProjectIODetail::SerializeSpawnCounters(scene)},
                 {"nextObjectId", scene.GetObjectStore().GetNextId()},
             }},
        };
}

bool SceneProjectIO::DeserializeScene(
    Scene& scene,
    ProjectEditorState& editorState,
    const json& root,
    const std::string& projectRoot,
    std::string& outError)
    {
        if (root.value("format", "") != SceneProjectIODetail::kFormatId)
        {
            outError = "Unrecognized project file format.";
            return false;
        }

        const int version = root.value("version", 0);
        if (version < 2 || version > SceneProjectIO::CurrentFormatVersion)
        {
            outError = "Unsupported project file version.";
            return false;
        }

        if (!root.contains("scene"))
        {
            outError = "Project file is missing scene data.";
            return false;
        }

        const json& sceneValue = root.at("scene");
        if (!sceneValue.contains("objects") || !sceneValue.at("objects").is_array())
        {
            outError = "Project file is missing scene objects.";
            return false;
        }

        scene.GetObjectStore().Clear();
        scene.GetMeshLibrary().ClearImportedMeshes();

        if (!SceneProjectIODetail::DeserializeObjects(
                scene,
                sceneValue.at("objects"),
                version,
                projectRoot,
                outError,
                nullptr,
                true))
        {
            scene.GetObjectStore().Clear();
            scene.GetMeshLibrary().ClearImportedMeshes();
            scene.ClearSelection();
            return false;
        }

        if (sceneValue.contains("editor"))
        {
            const json& editor = sceneValue.at("editor");
            scene.ClearSelection();
            scene.SetShowGrid(editor.value("showGrid", true));
            scene.SetShowLightGizmos(editor.value("showLightGizmos", true));
            SceneProjectIODetail::DeserializeEditorState(editor, editorState, projectRoot);
        }
        else
        {
            scene.ClearSelection();
            editorState = ProjectEditorState::CreateDefault();
        }

        if (sceneValue.contains("renderer"))
        {
            SceneProjectIODetail::DeserializeRenderer(scene, sceneValue.at("renderer"));
        }

        if (sceneValue.contains("spawnCounters"))
        {
            SceneProjectIODetail::DeserializeSpawnCounters(scene, sceneValue.at("spawnCounters"));
        }

        if (sceneValue.contains("nextObjectId"))
        {
            scene.GetObjectStore().SetNextId(sceneValue.at("nextObjectId").get<SceneObjectId>());
        }
        else
        {
            SceneProjectIODetail::EnsureNextObjectId(scene);
        }

        return true;
    }

bool SceneProjectIO::Save(
    const Scene& scene,
    const ProjectEditorState& editorState,
    const std::string& projectRoot,
    const std::string& projectFilePath,
    std::string& outError)
{
    outError.clear();

    try
    {
        const json root = SceneProjectIO::SerializeScene(scene, editorState, projectRoot);

        std::error_code error;
        const fs::path parentDirectory = fs::path(projectFilePath).parent_path();
        if (!parentDirectory.empty())
        {
            fs::create_directories(parentDirectory, error);
        }

        std::ofstream output(projectFilePath, std::ios::binary);
        if (!output)
        {
            outError = "Failed to open project file for writing.";
            return false;
        }

        output << root.dump(2);

        return true;
    }
    catch (const std::exception& exception)
    {
        outError = FormatExceptionContext("Failed to save project", exception);
        EngineLog::LogFailure("project-io", "Save", outError);
        return false;
    }
}

bool SceneProjectIO::Load(
    Scene& scene,
    ProjectEditorState& editorState,
    const std::string& projectRoot,
    const std::string& projectFilePath,
    std::string& outError)
{
    outError.clear();

    try
    {
        SetMaterialTexturePathResolver(projectRoot);

        ScopedNativeProgress progress("Loading Project", "Reading project file...");
        progress.SetProgress(0.04f);

        std::ifstream input(projectFilePath, std::ios::binary);
        if (!input)
        {
            outError = "Failed to open project file for reading.";
            return false;
        }

        json root;
        progress.SetMessage("Parsing project file...");
        progress.SetProgress(0.10f);
        input >> root;

        progress.SetMessage("Loading scene...");
        progress.SetProgress(SceneProjectIODetail::kProjectObjectLoadProgressStart);
        return SceneProjectIO::DeserializeScene(scene, editorState, root, projectRoot, outError);
    }
    catch (const std::exception& exception)
    {
        outError = FormatExceptionContext("Failed to load project file", exception);
        EngineLog::LogFailure("project-io", "Load", outError);
        return false;
    }
}

std::string SceneProjectIO::ResolveProjectPath(
    const std::string& projectRoot,
    const std::string& storedPath)
{
    if (storedPath.empty())
    {
        return {};
    }

    const fs::path stored(storedPath);
    if (stored.is_absolute())
    {
        return stored.generic_string();
    }

    if (!projectRoot.empty())
    {
        const fs::path rooted = fs::path(projectRoot) / stored;
        if (fs::exists(rooted))
        {
            return rooted.generic_string();
        }
    }

    if (fs::exists(stored))
    {
        return stored.generic_string();
    }

    return (fs::path(projectRoot) / stored).generic_string();
}

void SceneProjectIO::SetMaterialTexturePathResolver(const std::string& projectRoot)
{
    Material::SetTexturePathResolver([projectRoot](const std::string& storedPath) {
        return SceneProjectIO::ResolveProjectPath(projectRoot, storedPath);
    });
}
