#pragma once

#include <nlohmann/json.hpp>

#include <string>

class Scene;

namespace SceneContentSerialization
{
    nlohmann::json SerializeObjects(const Scene& scene, const std::string& projectRoot);
    bool DeserializeObjects(
        Scene& scene,
        const nlohmann::json& objects,
        int formatVersion,
        const std::string& projectRoot,
        std::string& outError);

    nlohmann::json SerializeSpawnCounters(const Scene& scene);
    void DeserializeSpawnCounters(Scene& scene, const nlohmann::json& spawnCounters);

    nlohmann::json SerializeSelection(const Scene& scene);
    void DeserializeSelection(Scene& scene, const nlohmann::json& selection);

    nlohmann::json SerializeSceneContent(const Scene& scene, const std::string& projectRoot);
    bool DeserializeSceneContent(
        Scene& scene,
        const nlohmann::json& content,
        int formatVersion,
        const std::string& projectRoot,
        std::string& outError);
}

class SceneDocument
{
public:
    static SceneDocument Capture(const Scene& scene, const std::string& projectRoot = {});
    bool Apply(Scene& scene, const std::string& projectRoot, std::string& outError, bool forUndoRedo = false) const;

    bool IsSameAs(const SceneDocument& other) const;

private:
    nlohmann::json m_content;
};
