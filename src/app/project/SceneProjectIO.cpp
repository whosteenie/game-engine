#include "app/project/SceneProjectIO.h"

#include "app/project/ProjectEditorState.h"
#include "app/project/SceneImportedMeshPool.h"
#include "engine/platform/NativeProgressWindow.h"

#include "app/scene/Scene.h"
#include "app/scene/SceneImportService.h"
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
#include "engine/rendering/RenderingPipelineCache.h"
#include "engine/assets/ModelImporter.h"
#include "engine/platform/EngineLog.h"
#include "engine/platform/ExceptionMessage.h"
#include "engine/platform/ProjectLoadBenchmark.h"
#include "engine/platform/ProjectLoadProgress.h"
#include "engine/platform/ProjectLoadTrace.h"
#include "engine/platform/SceneRenderTrace.h"
#include "engine/scene/SceneObject.h"
#include "engine/scene/SceneObjectId.h"
#include "engine/scene/ScenePrimitive.h"
#include "engine/lighting/DirectionalShadowSettings.h"
#include "engine/rendering/ScreenSpaceEffects.h"
#include "engine/rendering/ScreenSpaceEffectsSettings.h"
#include "engine/rendering/DxrSettings.h"
#include "engine/rendering/TextureSamplerSettings.h"
#include "engine/rendering/Texture.h"
#include "engine/assets/TextureCache.h"
#include "engine/rhi/GfxContext.h"
#include "engine/rhi/DlssContext.h"

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
    constexpr float kProjectObjectLoadProgressStart = ProjectLoadProgress::kDeserializingSceneStart;
    constexpr float kProjectObjectLoadProgressEnd = ProjectLoadProgress::kDeserializingSceneEnd;

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

    using LoadedImportedMeshMap = std::unordered_map<ImportMeshKey, Mesh*, ImportMeshKeyHash>;

    Mesh* AcquireImportedMesh(
        Scene& scene,
        const std::string& projectRoot,
        const std::string& storedAssetPath,
        int nodeIndex,
        std::unordered_map<std::string, ImportedAssetCacheEntry>& importCache,
        LoadedImportedMeshMap& loadedImportedMeshes,
        ImportedMeshReusePool* meshReusePool,
        std::string& outError)
    {
        const std::string resolvedPath = ResolveProjectPath(projectRoot, storedAssetPath);
        const ImportMeshKey meshKey{resolvedPath, nodeIndex};
        const auto loadedIterator = loadedImportedMeshes.find(meshKey);
        if (loadedIterator != loadedImportedMeshes.end())
        {
            return loadedIterator->second;
        }

        if (meshReusePool != nullptr)
        {
            const auto reuseIterator = meshReusePool->find(meshKey);
            if (reuseIterator != meshReusePool->end())
            {
                Mesh* reusedMesh = scene.GetMeshLibrary().AdoptImportedMesh(std::move(reuseIterator->second));
                meshReusePool->erase(reuseIterator);
                loadedImportedMeshes.emplace(meshKey, reusedMesh);
                return reusedMesh;
            }
        }

        ImportedAssetCacheEntry& cacheEntry = importCache[resolvedPath];
        if (!cacheEntry.loaded)
        {
            ProjectLoadBenchmark::ScopedPhase geometryLoadPhase(
                "project.deserialize.imported_model_geometry_load");
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

        Mesh* importedMesh = scene.GetMeshLibrary().AdoptImportedMesh(std::move(node.mesh));
        loadedImportedMeshes.emplace(meshKey, importedMesh);
        return importedMesh;
    }

    bool DeserializeObjectMesh(
        Scene& scene,
        const json& meshValue,
        const std::string& projectRoot,
        std::unordered_map<std::string, ImportedAssetCacheEntry>& importCache,
        LoadedImportedMeshMap& loadedImportedMeshes,
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
                loadedImportedMeshes,
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

    json SerializeRenderer(const Scene& scene)
    {
        const ScreenSpaceEffects& effects = scene.GetRenderer().GetScreenSpaceEffects();
        const DirectionalShadowSettings& shadowSettings = scene.GetRenderer().GetDirectionalShadowSettings();
        const DxrSettings& dxrSettings = scene.GetRenderer().GetDxrSettings();
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
            {"directionalShadow", shadowSettings.ToJson()},
            {"screenSpaceEffects", ScreenSpaceEffectsSettings::ToJson(effects)},
            {"dxr", dxrSettings.ToJson()},
        };
    }

    void ApplySkyboxDelta(EnvironmentMap& environmentMap, const json& skyboxValue)
    {
        if (skyboxValue.contains("enabled"))
        {
            environmentMap.SetEnabled(skyboxValue.at("enabled").get<bool>());
        }
        if (skyboxValue.contains("backgroundMode"))
        {
            environmentMap.SetBackgroundMode(static_cast<EnvironmentBackgroundMode>(
                skyboxValue.at("backgroundMode").get<int>()));
        }
        if (skyboxValue.contains("hdrPath"))
        {
            environmentMap.SetHdrPath(skyboxValue.at("hdrPath").get<std::string>());
        }
        if (skyboxValue.contains("rotationDegrees"))
        {
            environmentMap.SetRotationDegrees(skyboxValue.at("rotationDegrees").get<float>());
        }
        if (skyboxValue.contains("exposure"))
        {
            environmentMap.SetExposure(skyboxValue.at("exposure").get<float>());
        }
        if (skyboxValue.contains("iblCubemapResolution"))
        {
            environmentMap.SetIblCubemapResolution(static_cast<EnvironmentIblCubemapResolution>(
                skyboxValue.at("iblCubemapResolution").get<int>()));
        }
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

    void ApplyRendererSettingsDelta(Scene& scene, const json& delta, const bool deferIfGpuNotReady = true)
    {
        if (!delta.is_object())
        {
            return;
        }

        SceneRenderer& renderer = scene.GetRenderer();

        // CPU-safe settings: no D3D12 resources required (applied even when deferring).
        if (delta.contains("directionalShadow"))
        {
            renderer.GetDirectionalShadowSettings().ApplyFromJson(delta.at("directionalShadow"));
        }

        if (delta.contains("dxr"))
        {
            DxrSettings& dxrSettings = renderer.GetDxrSettings();
            dxrSettings.ApplyFromJson(delta.at("dxr"));
            if (GfxContext::Get().IsInitialized())
            {
                dxrSettings.ClampToHardwareWithLogging(GfxContext::Get().IsRaytracingSupported());
            }
        }

        const auto deferPendingRendererSettings = [&]()
        {
            renderer.MergePendingRendererSettings(delta);
            ProjectLoadTrace::Step("renderer settings deferred (GPU not ready)");
            scene.MarkDirty();
        };

        // During project load the GPU path is not ready yet; never touch D3D12 resources here.
        if (deferIfGpuNotReady && !renderer.IsGpuResourcesReady())
        {
            // Geometry MSAA must be on GfxContext before the first EnsureGpuResources so we
            // don't init at 1x and immediately tear down/reinit when deferred settings apply.
            const ScreenSpaceEffectsSettings::LoadedAntiAliasingSettings pendingAaSettings =
                ScreenSpaceEffectsSettings::ResolveLoadedAntiAliasingSettings(delta);
            GfxContext::Get().SetActiveMsaaSampleCount(pendingAaSettings.msaaSampleCount);

            deferPendingRendererSettings();
            return;
        }

        ScreenSpaceEffectsSettings::LoadedAntiAliasingSettings aaToApply{};
        bool applyAaSettings = false;
        if (delta.contains("screenSpaceEffects"))
        {
            const json& effectsValue = delta.at("screenSpaceEffects");
            if (effectsValue.contains("antiAliasingMode") || effectsValue.contains("msaaSampleCount"))
            {
                AntiAliasingMode aaMode = AntiAliasingMode::None;
                int msaaSampleCount = 1;
                if (renderer.IsGpuResourcesReady())
                {
                    const ScreenSpaceEffects& currentEffects = renderer.GetScreenSpaceEffects();
                    aaMode = currentEffects.GetAntiAliasingMode();
                    msaaSampleCount = currentEffects.GetMsaaSampleCount();
                }
                aaToApply = ScreenSpaceEffectsSettings::ResolveAntiAliasingDelta(
                    effectsValue,
                    aaMode,
                    msaaSampleCount);
                applyAaSettings = true;
                if (aaToApply.msaaSampleCount > 1)
                {
                    RenderingPipelineCache::InvalidateAll();
                }
                renderer.PrepareGpuResourcesForGeometryMsaa(aaToApply.msaaSampleCount);
                if (renderer.IsGpuResourcesReady() && aaToApply.msaaSampleCount > 1)
                {
                    scene.InvalidateAllMaterialCachedShaders();
                }
            }
        }

        if (GfxContext::Get().IsInitialized())
        {
            renderer.PrepareGpuResources();
        }

        const bool gpuReady = renderer.IsGpuResourcesReady();
        if (deferIfGpuNotReady && !gpuReady)
        {
            deferPendingRendererSettings();
            return;
        }

        if (gpuReady)
        {
            if (applyAaSettings)
            {
                ScreenSpaceEffects& effects = renderer.GetScreenSpaceEffects();
                effects.SetAntiAliasingMode(aaToApply.antiAliasingMode);
                effects.SetMsaaSampleCount(aaToApply.msaaSampleCount);
            }

            if (delta.contains("environmentIntensity"))
            {
                renderer.GetIBL().SetEnvironmentIntensity(delta.at("environmentIntensity").get<float>());
            }

            if (delta.contains("skybox"))
            {
                ApplySkyboxDelta(renderer.GetEnvironmentMap(), delta.at("skybox"));
            }

            if (delta.contains("textureFilterMode"))
            {
                renderer.SetTextureFilterMode(TextureFilterModeFromString(
                    delta.at("textureFilterMode").get<std::string>()));
            }
            if (delta.contains("textureAnisotropy"))
            {
                renderer.SetTextureAnisotropy(delta.at("textureAnisotropy").get<float>());
            }
            if (delta.contains("textureMipBias"))
            {
                renderer.SetTextureMipBias(delta.at("textureMipBias").get<float>());
            }

            if (delta.contains("screenSpaceEffects"))
            {
                ScreenSpaceEffectsSettings::ApplyFromJson(
                    renderer.GetScreenSpaceEffects(),
                    delta.at("screenSpaceEffects"));
            }
        }

        scene.MarkDirty();
    }

    void MergeRendererSettings(json& target, const json& delta)
    {
        if (!delta.is_object())
        {
            target = delta;
            return;
        }

        if (!target.is_object())
        {
            target = json::object();
        }

        for (const auto& [key, value] : delta.items())
        {
            if (value.is_object() && target.contains(key) && target.at(key).is_object())
            {
                MergeRendererSettings(target[key], value);
            }
            else
            {
                target[key] = value;
            }
        }
    }

    void ApplyDeferredRendererSettings(Scene& scene)
    {
        SceneRenderer& renderer = scene.GetRenderer();
        if (!renderer.HasPendingRendererSettings())
        {
            return;
        }

        SceneRenderTrace::Scope applyScope("ApplyDeferredRendererSettings");
        try
        {
            const json pending = renderer.TakePendingRendererSettings();
            ApplyRendererSettingsDelta(scene, pending, false);
            applyScope.Success();
        }
        catch (const std::exception& exception)
        {
            EngineLog::Error(
                "load",
                std::string("ApplyDeferredRendererSettings failed: ") + exception.what());
            throw;
        }
    }

    void DeserializeRenderer(Scene& scene, const json& rendererValue)
    {
        ProjectLoadTrace::Scope rendererScope("deserialize renderer settings");
        try
        {
            ApplyRendererSettingsDelta(scene, rendererValue);
            if (scene.GetRenderer().HasPendingRendererSettings())
            {
                ProjectLoadTrace::Step("renderer settings pending until first GPU frame");
            }
            rendererScope.Success();
        }
        catch (const std::exception& exception)
        {
            EngineLog::Error(
                "load",
                std::string("deserialize renderer settings failed: ") + exception.what());
            throw;
        }
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
            {"performance",
             json{
                 {"gpuPassSmoothing", editorState.performanceGpuPassSmoothing},
                 {"cpuPassSmoothing", editorState.performanceCpuPassSmoothing},
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

        if (editorValue.contains("performance"))
        {
            const json& performanceValue = editorValue.at("performance");
            editorState.performanceGpuPassSmoothing =
                performanceValue.value("gpuPassSmoothing", editorState.performanceGpuPassSmoothing);
            editorState.performanceCpuPassSmoothing =
                performanceValue.value("cpuPassSmoothing", editorState.performanceCpuPassSmoothing);
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
        ProjectLoadBenchmark::ScopedPhase deserializeObjectsPhase("project.deserialize.objects");
        std::unordered_map<std::string, ImportedAssetCacheEntry> importCache;
        LoadedImportedMeshMap loadedImportedMeshes;
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
        ProjectLoadTrace::Step(
            "deserialize scene objects (count=" + std::to_string(objectCount) + ")");
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
                    loadedImportedMeshes,
                    meshReusePool,
                    mesh,
                    importAssetPath,
                    importNodeIndex,
                    outError))
            {
                return false;
            }

            ProjectLoadBenchmark::ScopedPhase objectRecordPhase("project.deserialize.object_record");
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
                ProjectLoadTrace::Step("deserialize scene objects exception");
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

        for (auto& [importPath, cacheEntry] : importCache)
        {
            if (!cacheEntry.loaded || !cacheEntry.errorMessage.empty())
            {
                continue;
            }

            scene.GetImportService().CacheLoadedProjectModel(
                scene,
                importPath,
                std::move(cacheEntry.model));
        }

        ProjectLoadTrace::Step("deserialize scene objects ok");
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
        scene.ClearImportedModelCache();
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
            scene.ClearImportedModelCache();
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
        ProjectLoadTrace::Scope deserializeScope("DeserializeScene");
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

        ImportedMeshReusePool reusableMeshes;
        scene.GetMeshLibrary().HarvestImportedMeshes(scene.GetObjects(), reusableMeshes);
        scene.GetObjectStore().Clear();
        scene.ClearImportedModelCache();
        scene.GetMeshLibrary().ClearImportedMeshes();
        ProjectLoadTrace::Step("scene stores cleared");

        if (!SceneProjectIODetail::DeserializeObjects(
                scene,
                sceneValue.at("objects"),
                version,
                projectRoot,
                outError,
                &reusableMeshes,
                true))
        {
            scene.GetObjectStore().Clear();
            scene.ClearImportedModelCache();
            scene.GetMeshLibrary().ClearImportedMeshes();
            scene.ClearSelection();
            return false;
        }

        if (sceneValue.contains("editor"))
        {
            ProjectLoadTrace::Step("deserialize editor state");
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
        else
        {
            ProjectLoadTrace::Step("no renderer block in project file");
        }

        if (sceneValue.contains("spawnCounters"))
        {
            ProjectLoadTrace::Step("deserialize spawn counters");
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

        deserializeScope.Success();
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
        ProjectLoadBenchmark::ScopedPhase loadProjectFilePhase("project.file.total");
        ProjectLoadTrace::Step("set material texture resolver");
        SetMaterialTexturePathResolver(projectRoot);

        ScopedNativeProgress progress("Loading Project", "Reading project file...");
        progress.SetProgress(ProjectLoadProgress::kReadingProjectFile);

        json root;
        {
            ProjectLoadBenchmark::ScopedPhase parseProjectFilePhase("project.file.read_and_parse_json");
            ProjectLoadTrace::Step("open project file");
            std::ifstream input(projectFilePath, std::ios::binary);
            if (!input)
            {
                outError = "Failed to open project file for reading.";
                ProjectLoadTrace::Step("open project file failed");
                return false;
            }

            progress.SetMessage("Parsing project file...");
            progress.SetProgress(ProjectLoadProgress::kParsingProjectFile);
            ProjectLoadTrace::Step("parse project json");
            input >> root;
            ProjectLoadTrace::Step("parse project json ok");
        }

        progress.SetMessage("Loading scene...");
        progress.SetProgress(SceneProjectIODetail::kProjectObjectLoadProgressStart);
        ProjectLoadBenchmark::ScopedPhase deserializeScenePhase("project.deserialize.scene");
        return SceneProjectIO::DeserializeScene(scene, editorState, root, projectRoot, outError);
    }
    catch (const std::exception& exception)
    {
        ProjectLoadTrace::Step("SceneProjectIO::Load exception");
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
