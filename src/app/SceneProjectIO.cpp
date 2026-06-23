#include "app/SceneProjectIO.h"

#include "app/ProjectEditorState.h"
#include "app/SceneImportedMeshPool.h"
#include "engine/NativeProgressWindow.h"

#include "app/Scene.h"
#include "engine/Constants.h"
#include "engine/IBL.h"
#include "engine/CameraComponent.h"
#include "engine/ColliderComponent.h"
#include "engine/LightComponent.h"
#include "engine/RigidBodyComponent.h"
#include "engine/Material.h"
#include "engine/Mesh.h"
#include "engine/ModelImporter.h"
#include "engine/SceneObject.h"
#include "engine/SceneObjectId.h"
#include "engine/ScenePrimitive.h"
#include "engine/DirectionalShadowSettings.h"
#include "engine/ScreenSpaceEffects.h"
#include "engine/Texture.h"
#include "engine/TextureCache.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <unordered_map>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace SceneProjectIODetail
{
    constexpr const char* kFormatId = "game-engine-project";

    json Vec3ToJson(const glm::vec3& value)
    {
        return json::array({value.x, value.y, value.z});
    }

    glm::vec3 Vec3FromJson(const json& value)
    {
        return glm::vec3(value.at(0).get<float>(), value.at(1).get<float>(), value.at(2).get<float>());
    }

    json QuatToJson(const glm::quat& value)
    {
        return json::array({value.w, value.x, value.y, value.z});
    }

    glm::quat QuatFromJson(const json& value)
    {
        return glm::quat(
            value.at(0).get<float>(),
            value.at(1).get<float>(),
            value.at(2).get<float>(),
            value.at(3).get<float>());
    }

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

    const char* LightTypeToString(LightType type)
    {
        switch (type)
        {
        case LightType::Directional:
            return "Directional";
        case LightType::Point:
            return "Point";
        case LightType::Spot:
            return "Spot";
        }

        return "Point";
    }

    bool LightTypeFromString(const std::string& value, LightType& outType)
    {
        if (value == "Directional")
        {
            outType = LightType::Directional;
            return true;
        }

        if (value == "Point")
        {
            outType = LightType::Point;
            return true;
        }

        if (value == "Spot")
        {
            outType = LightType::Spot;
            return true;
        }

        return false;
    }

    json SerializeLight(const LightComponent& light)
    {
        return json{
            {"type", LightTypeToString(light.type)},
            {"color", Vec3ToJson(light.color)},
            {"intensity", light.intensity},
            {"constantAttenuation", light.constantAttenuation},
            {"linearAttenuation", light.linearAttenuation},
            {"quadraticAttenuation", light.quadraticAttenuation},
            {"range", light.range},
            {"innerCutoffDegrees", light.innerCutoffDegrees},
            {"outerCutoffDegrees", light.outerCutoffDegrees},
            {"castsShadow", light.castsShadow},
        };
    }

    LightComponent DeserializeLight(const json& value)
    {
        LightComponent light;
        LightType type = LightType::Point;
        if (value.contains("type"))
        {
            LightTypeFromString(value.at("type").get<std::string>(), type);
        }

        light = MakeDefaultLightComponent(type);
        light.color = Vec3FromJson(value.at("color"));
        light.intensity = value.at("intensity").get<float>();
        light.constantAttenuation = value.value("constantAttenuation", light.constantAttenuation);
        light.linearAttenuation = value.value("linearAttenuation", light.linearAttenuation);
        light.quadraticAttenuation = value.value("quadraticAttenuation", light.quadraticAttenuation);
        light.range = value.value("range", light.range);
        light.innerCutoffDegrees = value.value("innerCutoffDegrees", light.innerCutoffDegrees);
        light.outerCutoffDegrees = value.value("outerCutoffDegrees", light.outerCutoffDegrees);
        light.castsShadow = value.value("castsShadow", light.castsShadow);
        return light;
    }

    json SerializeCamera(const CameraComponent& camera)
    {
        return json{
            {"fovDegrees", camera.fovDegrees},
            {"nearPlane", camera.nearPlane},
            {"farPlane", camera.farPlane},
            {"enabled", camera.enabled},
            {"depth", camera.depth},
            {"isMain", camera.isMain},
        };
    }

    CameraComponent DeserializeCamera(const json& value)
    {
        CameraComponent camera = MakeDefaultCameraComponent();
        camera.fovDegrees = value.value("fovDegrees", camera.fovDegrees);
        camera.nearPlane = value.value("nearPlane", camera.nearPlane);
        camera.farPlane = value.value("farPlane", camera.farPlane);
        camera.enabled = value.value("enabled", camera.enabled);
        camera.depth = value.value("depth", camera.depth);
        camera.isMain = value.value("isMain", camera.isMain);
        return camera;
    }

    const char* ColliderShapeToString(ColliderShape shape)
    {
        switch (shape)
        {
        case ColliderShape::Box:
            return "box";
        case ColliderShape::Sphere:
            return "sphere";
        }

        return "box";
    }

    bool ColliderShapeFromString(const std::string& value, ColliderShape& outShape)
    {
        if (value == "box")
        {
            outShape = ColliderShape::Box;
            return true;
        }

        if (value == "sphere")
        {
            outShape = ColliderShape::Sphere;
            return true;
        }

        return false;
    }

    json SerializeCollider(const ColliderComponent& collider)
    {
        return json{
            {"shape", ColliderShapeToString(collider.shape)},
            {"offset", Vec3ToJson(collider.offset)},
            {"halfExtents", Vec3ToJson(collider.halfExtents)},
            {"radius", collider.radius},
            {"isTrigger", collider.isTrigger},
        };
    }

    ColliderComponent DeserializeCollider(const json& value)
    {
        ColliderComponent collider = MakeDefaultColliderComponent();
        ColliderShape shape = ColliderShape::Box;
        if (value.contains("shape"))
        {
            ColliderShapeFromString(value.at("shape").get<std::string>(), shape);
        }

        collider.shape = shape;
        if (value.contains("offset"))
        {
            collider.offset = Vec3FromJson(value.at("offset"));
        }
        if (value.contains("halfExtents"))
        {
            collider.halfExtents = Vec3FromJson(value.at("halfExtents"));
        }
        collider.radius = value.value("radius", collider.radius);
        collider.isTrigger = value.value("isTrigger", collider.isTrigger);
        return collider;
    }

    json SerializeRigidBody(const RigidBodyComponent& rigidBody)
    {
        return json{
            {"mass", rigidBody.mass},
            {"useGravity", rigidBody.useGravity},
            {"isKinematic", rigidBody.isKinematic},
        };
    }

    RigidBodyComponent DeserializeRigidBody(const json& value)
    {
        RigidBodyComponent rigidBody = MakeDefaultRigidBodyComponent();
        rigidBody.mass = value.value("mass", rigidBody.mass);
        rigidBody.useGravity = value.value("useGravity", rigidBody.useGravity);
        rigidBody.isKinematic = value.value("isKinematic", rigidBody.isKinematic);
        return rigidBody;
    }

    json SerializeMaterial(const Material& material, const std::string& projectRoot)
    {
        json maps = json::object();
        if (material.HasAlbedoMap())
        {
            const std::string mapPath = ToProjectRelativePath(projectRoot, material.GetAlbedoMapPath());
            if (!mapPath.empty())
            {
                maps["albedo"] = mapPath;
            }
        }

        if (material.HasNormalMap())
        {
            const std::string mapPath = ToProjectRelativePath(projectRoot, material.GetNormalMapPath());
            if (!mapPath.empty())
            {
                maps["normal"] = mapPath;
            }
        }

        if (material.HasAoMap())
        {
            const std::string mapPath = ToProjectRelativePath(projectRoot, material.GetAoMapPath());
            if (!mapPath.empty())
            {
                maps["ao"] = mapPath;
            }
        }

        if (material.HasRoughnessMap())
        {
            const std::string mapPath = ToProjectRelativePath(projectRoot, material.GetRoughnessMapPath());
            if (!mapPath.empty())
            {
                maps["roughness"] = mapPath;
                maps["metallicRoughness"] = material.HasMetallicRoughnessMap();
            }
        }

        return json{
            {"albedo", Vec3ToJson(material.GetAlbedo())},
            {"roughness", material.GetRoughness()},
            {"metallic", material.GetMetallic()},
            {"doubleSided", material.IsDoubleSided()},
            {"maps", maps},
            {"texCoordSets",
             json{
                 {"albedo", material.GetAlbedoTexCoordSet()},
                 {"normal", material.GetNormalTexCoordSet()},
                 {"ao", material.GetAoTexCoordSet()},
                 {"roughness", material.GetRoughnessTexCoordSet()},
             }},
        };
    }

    std::unique_ptr<Material> DeserializeMaterial(const json& value, const std::string& projectRoot)
    {
        const glm::vec3 albedo = Vec3FromJson(value.at("albedo"));
        const float roughness = value.at("roughness").get<float>();
        const float metallic = value.at("metallic").get<float>();

        auto material = std::make_unique<Material>(
            EngineConstants::LitVertexShader,
            EngineConstants::PbrFragmentShader,
            albedo,
            roughness,
            metallic);

        material->SetDoubleSided(value.value("doubleSided", false));

        if (value.contains("texCoordSets"))
        {
            const json& texCoordSets = value.at("texCoordSets");
            material->SetAlbedoTexCoordSet(texCoordSets.value("albedo", 0));
            material->SetNormalTexCoordSet(texCoordSets.value("normal", 0));
            material->SetAoTexCoordSet(texCoordSets.value("ao", 0));
            material->SetRoughnessTexCoordSet(texCoordSets.value("roughness", 0));
        }

        if (!value.contains("maps"))
        {
            return material;
        }

        const json& maps = value.at("maps");
        TextureCache& cache = TextureCache::Get();

        auto tryLoadMap =
            [&](const char* key, TextureColorSpace colorSpace, auto setter) {
                if (!maps.contains(key))
                {
                    return;
                }

                const std::string storedPath = maps.at(key).get<std::string>();
                if (storedPath.empty())
                {
                    return;
                }

                const std::string resolvedPath = ResolveProjectPath(projectRoot, storedPath);
                try
                {
                    std::shared_ptr<Texture> texture = cache.Load(resolvedPath.c_str(), colorSpace);
                    setter(std::move(texture), ToProjectRelativePath(projectRoot, resolvedPath));
                }
                catch (const std::exception&)
                {
                }
            };

        tryLoadMap(
            "albedo",
            TextureColorSpace::SRGB,
            [&](std::shared_ptr<Texture> texture, const std::string& path) {
                material->SetAlbedoMap(std::move(texture), path);
            });
        tryLoadMap(
            "normal",
            TextureColorSpace::Linear,
            [&](std::shared_ptr<Texture> texture, const std::string& path) {
                material->SetNormalMap(std::move(texture), path);
            });
        tryLoadMap(
            "ao",
            TextureColorSpace::Linear,
            [&](std::shared_ptr<Texture> texture, const std::string& path) {
                material->SetAoMap(std::move(texture), path);
            });

        if (maps.contains("roughness"))
        {
            const std::string storedPath = maps.at("roughness").get<std::string>();
            if (!storedPath.empty())
            {
                const std::string resolvedPath = ResolveProjectPath(projectRoot, storedPath);
                const bool metallicRoughness = maps.value("metallicRoughness", false);
                try
                {
                    std::shared_ptr<Texture> texture = cache.Load(resolvedPath.c_str(), TextureColorSpace::Linear);
                    const std::string relativePath = ToProjectRelativePath(projectRoot, resolvedPath);
                    if (metallicRoughness)
                    {
                        material->SetMetallicRoughnessMap(
                            std::move(texture),
                            material->GetRoughnessTexCoordSet(),
                            relativePath);
                    }
                    else
                    {
                        material->SetRoughnessMap(std::move(texture), relativePath);
                    }
                }
                catch (const std::exception&)
                {
                }
            }
        }

        return material;
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
            if (mesh == scene.GetMeshForPrimitive(primitive))
            {
                return primitive;
            }
        }

        return std::nullopt;
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
                Mesh* reusedMesh = scene.AdoptImportedMesh(std::move(reuseIterator->second));
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

        return scene.AdoptImportedMesh(std::move(node.mesh));
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

            outMesh = scene.GetMeshForPrimitive(primitive);
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
        const ScreenSpaceEffects& effects = scene.GetScreenSpaceEffects();
        const DirectionalShadowSettings& shadowSettings = scene.GetDirectionalShadowSettings();
        return json{
            {"environmentIntensity", scene.GetIBL().GetEnvironmentIntensity()},
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
                 {"shadowBlurEnabled", shadowSettings.GetShadowBlurEnabled()},
                 {"shadowBlurRadius", shadowSettings.GetShadowBlurRadius()},
             }},
            {"screenSpaceEffects",
             json{
                 {"enabled", effects.IsEnabled()},
                 {"ssaoEnabled", effects.IsSsaoEnabled()},
                 {"ssaoRadius", effects.GetSsaoRadius()},
                 {"ssaoBias", effects.GetSsaoBias()},
                 {"ssaoPower", effects.GetSsaoPower()},
                 {"aoStrength", effects.GetAoStrength()},
                 {"exposure", effects.GetExposure()},
                 {"tonemapMode", TonemapModeToString(effects.GetTonemapMode())},
                 {"bloomEnabled", effects.IsBloomEnabled()},
                 {"bloomThreshold", effects.GetBloomThreshold()},
                 {"bloomSoftKnee", effects.GetBloomSoftKnee()},
                 {"bloomIntensity", effects.GetBloomIntensity()},
                 {"bloomBlurRadius", effects.GetBloomBlurRadius()},
             }},
        };
    }

    void DeserializeRenderer(Scene& scene, const json& rendererValue)
    {
        scene.GetIBL().SetEnvironmentIntensity(
            rendererValue.value("environmentIntensity", scene.GetIBL().GetEnvironmentIntensity()));

        if (rendererValue.contains("directionalShadow"))
        {
            const json& shadowValue = rendererValue.at("directionalShadow");
            DirectionalShadowSettings& shadowSettings = scene.GetDirectionalShadowSettings();
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
            shadowSettings.SetShadowBlurEnabled(
                shadowValue.value("shadowBlurEnabled", shadowSettings.GetShadowBlurEnabled()));
            shadowSettings.SetShadowBlurRadius(
                shadowValue.value("shadowBlurRadius", shadowSettings.GetShadowBlurRadius()));
        }

        if (!rendererValue.contains("screenSpaceEffects"))
        {
            return;
        }

        const json& effectsValue = rendererValue.at("screenSpaceEffects");
        ScreenSpaceEffects& effects = scene.GetScreenSpaceEffects();
        effects.SetEnabled(effectsValue.value("enabled", effects.IsEnabled()));
        effects.SetSsaoEnabled(effectsValue.value("ssaoEnabled", effects.IsSsaoEnabled()));
        effects.SetSsaoRadius(effectsValue.value("ssaoRadius", effects.GetSsaoRadius()));
        effects.SetSsaoBias(effectsValue.value("ssaoBias", effects.GetSsaoBias()));
        effects.SetSsaoPower(effectsValue.value("ssaoPower", effects.GetSsaoPower()));
        effects.SetAoStrength(effectsValue.value("aoStrength", effects.GetAoStrength()));
        effects.SetExposure(effectsValue.value("exposure", effects.GetExposure()));
        effects.SetTonemapMode(
            TonemapModeFromString(effectsValue.value("tonemapMode", TonemapModeToString(effects.GetTonemapMode()))));
        effects.SetBloomEnabled(effectsValue.value("bloomEnabled", effects.IsBloomEnabled()));
        effects.SetBloomThreshold(effectsValue.value("bloomThreshold", effects.GetBloomThreshold()));
        effects.SetBloomSoftKnee(effectsValue.value("bloomSoftKnee", effects.GetBloomSoftKnee()));
        effects.SetBloomIntensity(effectsValue.value("bloomIntensity", effects.GetBloomIntensity()));
        effects.SetBloomBlurRadius(effectsValue.value("bloomBlurRadius", effects.GetBloomBlurRadius()));
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

    fs::path GetEditorLayoutPath(const std::string& projectRoot)
    {
        return fs::path(projectRoot) / ".editor" / "imgui.ini";
    }

    bool SaveEditorLayout(const std::string& projectRoot)
    {
        std::size_t iniSize = 0;
        const char* iniData = ImGui::SaveIniSettingsToMemory(&iniSize);
        if (iniData == nullptr || iniSize == 0)
        {
            return true;
        }

        const fs::path layoutPath = GetEditorLayoutPath(projectRoot);
        std::error_code error;
        fs::create_directories(layoutPath.parent_path(), error);

        std::ofstream output(layoutPath, std::ios::binary);
        if (!output)
        {
            return false;
        }

        output.write(iniData, static_cast<std::streamsize>(iniSize));
        return static_cast<bool>(output);
    }

    bool LoadEditorLayout(const std::string& projectRoot)
    {
        const fs::path layoutPath = GetEditorLayoutPath(projectRoot);
        if (!fs::exists(layoutPath))
        {
            return true;
        }

        std::ifstream input(layoutPath, std::ios::binary);
        if (!input)
        {
            return false;
        }

        const std::string iniData(
            (std::istreambuf_iterator<char>(input)),
            std::istreambuf_iterator<char>());
        if (iniData.empty())
        {
            return true;
        }

        ImGui::LoadIniSettingsFromMemory(iniData.c_str(), iniData.size());
        return true;
    }

    bool DeleteEditorLayout(const std::string& projectRoot)
    {
        const fs::path layoutPath = GetEditorLayoutPath(projectRoot);
        std::error_code error;
        fs::remove(layoutPath, error);
        return true;
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
                entry["material"] = SerializeMaterial(object.GetMaterial(), projectRoot);
            }

            if (object.HasLight())
            {
                entry["light"] = SerializeLight(object.GetLight());
            }

            if (object.HasCamera())
            {
                entry["camera"] = SerializeCamera(object.GetCamera());
            }

            if (object.HasRigidBody())
            {
                entry["rigidBody"] = SerializeRigidBody(object.GetRigidBody());
            }

            if (object.HasCollider())
            {
                entry["collider"] = SerializeCollider(object.GetCollider());
            }

            objects.push_back(std::move(entry));
        }

        return objects;
    }

    json SerializeSpawnCounters(const Scene& scene)
    {
        const Scene::SpawnCounters counters = scene.GetSpawnCounters();
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
        Scene::SpawnCounters values = scene.GetSpawnCounters();
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
        scene.SetSpawnCounters(values);
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

        const std::size_t objectCount = objectValues.size();
        std::size_t objectIndex = 0;
        for (const json& objectValue : objectValues)
        {
            ++objectIndex;
            if (showProgress)
            {
                const float loadProgress =
                    objectCount > 0 ? static_cast<float>(objectIndex) / static_cast<float>(objectCount) : 1.0f;
                NativeProgressWindow::Instance().SetMessage(
                    "Loading scene objects (" + std::to_string(objectIndex) + "/"
                    + std::to_string(objectCount) + ")...");
                NativeProgressWindow::Instance().SetProgress(0.05f + (loadProgress * 0.95f));
            }

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
                material = DeserializeMaterial(objectValue.at("material"), projectRoot);
            }

            std::optional<LightComponent> light;
            if (objectValue.contains("light"))
            {
                light = DeserializeLight(objectValue.at("light"));
            }

            std::optional<CameraComponent> camera;
            if (objectValue.contains("camera"))
            {
                camera = DeserializeCamera(objectValue.at("camera"));
            }

            std::optional<RigidBodyComponent> rigidBody;
            if (objectValue.contains("rigidBody"))
            {
                rigidBody = DeserializeRigidBody(objectValue.at("rigidBody"));
            }

            std::optional<ColliderComponent> collider;
            if (objectValue.contains("collider"))
            {
                collider = DeserializeCollider(objectValue.at("collider"));
            }

            Transform transform;
            const json& transformValue = objectValue.at("transform");
            transform.position = Vec3FromJson(transformValue.at("position"));
            transform.rotation = QuatFromJson(transformValue.at("rotation"));
            transform.scale = Vec3FromJson(transformValue.at("scale"));

            const json& boundsValue = objectValue.at("bounds");
            const glm::vec3 boundsMin = Vec3FromJson(boundsValue.at("min"));
            const glm::vec3 boundsMax = Vec3FromJson(boundsValue.at("max"));

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

            if (formatVersion >= 3 && objectId != kInvalidSceneObjectId)
            {
                scene.RegisterObjectId(objectId);
            }
            else
            {
                scene.FinalizeNewObject(createdObject);
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

        if (scene.GetNextObjectIdValue() <= maxId)
        {
            scene.SetNextObjectIdValue(maxId + 1);
        }
    }

    json SerializeSceneContent(const Scene& scene, const std::string& projectRoot)
    {
        return json{
            {"objects", SerializeObjects(scene, projectRoot)},
            {"spawnCounters", SerializeSpawnCounters(scene)},
            {"nextObjectId", scene.GetNextObjectIdValue()},
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
            scene.HarvestImportedMeshes(harvestedMeshes);
            meshReusePool = &harvestedMeshes;
        }

        scene.ClearSceneObjectsAndImports();

        if (!DeserializeObjects(
                scene,
                content.at("objects"),
                formatVersion,
                projectRoot,
                outError,
                meshReusePool,
                showProgress))
        {
            return false;
        }

        if (content.contains("spawnCounters"))
        {
            DeserializeSpawnCounters(scene, content.at("spawnCounters"));
        }

        if (content.contains("nextObjectId"))
        {
            scene.SetNextObjectIdValue(content.at("nextObjectId").get<SceneObjectId>());
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
                 {"nextObjectId", scene.GetNextObjectIdValue()},
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

        scene.ClearSceneObjectsAndImports();

        if (!SceneProjectIODetail::DeserializeObjects(
                scene,
                sceneValue.at("objects"),
                version,
                projectRoot,
                outError,
                nullptr,
                true))
        {
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
            scene.SetNextObjectIdValue(sceneValue.at("nextObjectId").get<SceneObjectId>());
        }
        else
        {
            SceneProjectIODetail::EnsureNextObjectId(scene);
        }

        if (!SceneProjectIODetail::LoadEditorLayout(projectRoot))
        {
            outError = "Failed to load editor layout.";
            return false;
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

        if (!SceneProjectIODetail::SaveEditorLayout(projectRoot))
        {
            outError = "Failed to save editor layout.";
            return false;
        }

        return true;
    }
    catch (const std::exception& exception)
    {
        outError = exception.what();
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
        ScopedNativeProgress progress("Loading Project", "Reading project file...");

        std::ifstream input(projectFilePath, std::ios::binary);
        if (!input)
        {
            outError = "Failed to open project file for reading.";
            return false;
        }

        json root;
        input >> root;

        progress.SetMessage("Loading scene...");
        progress.SetProgress(0.05f);
        return SceneProjectIO::DeserializeScene(scene, editorState, root, projectRoot, outError);
    }
    catch (const std::exception& exception)
    {
        outError = exception.what();
        return false;
    }
}

bool SceneProjectIO::SaveEditorLayout(const std::string& projectRoot)
{
    return SceneProjectIODetail::SaveEditorLayout(projectRoot);
}

bool SceneProjectIO::LoadEditorLayout(const std::string& projectRoot)
{
    return SceneProjectIODetail::LoadEditorLayout(projectRoot);
}

bool SceneProjectIO::DeleteEditorLayout(const std::string& projectRoot)
{
    return SceneProjectIODetail::DeleteEditorLayout(projectRoot);
}
