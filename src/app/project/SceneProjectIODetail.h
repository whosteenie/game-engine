#pragma once

#include "app/project/SceneImportedMeshPool.h"

#include <nlohmann/json_fwd.hpp>

#include <string>

class Scene;

namespace SceneProjectIODetail
{
    nlohmann::json SerializeObjects(const Scene& scene, const std::string& projectRoot);
    bool DeserializeObjects(
        Scene& scene,
        const nlohmann::json& objects,
        int formatVersion,
        const std::string& projectRoot,
        std::string& outError,
        ImportedMeshReusePool* meshReusePool = nullptr,
        bool showProgress = true);

    nlohmann::json SerializeSpawnCounters(const Scene& scene);
    void DeserializeSpawnCounters(Scene& scene, const nlohmann::json& spawnCounters);

    nlohmann::json SerializeSelection(const Scene& scene);
    void DeserializeSelection(Scene& scene, const nlohmann::json& selection);

    void EnsureNextObjectId(Scene& scene);

    nlohmann::json SerializeSceneContent(const Scene& scene, const std::string& projectRoot);
    bool DeserializeSceneContent(
        Scene& scene,
        const nlohmann::json& content,
        int formatVersion,
        const std::string& projectRoot,
        std::string& outError,
        ImportedMeshReusePool* meshReusePool = nullptr,
        bool showProgress = true);

    nlohmann::json SerializeRenderer(const Scene& scene);
    void DeserializeRenderer(Scene& scene, const nlohmann::json& rendererValue);
    void ApplyRendererSettingsDelta(Scene& scene, const nlohmann::json& delta, bool deferIfGpuNotReady = true);
    void MergeRendererSettings(nlohmann::json& target, const nlohmann::json& delta);
    void ApplyDeferredRendererSettings(Scene& scene);
}
