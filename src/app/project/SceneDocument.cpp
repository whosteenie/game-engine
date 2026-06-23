#include "app/project/SceneDocument.h"

#include "app/scene/Scene.h"
#include "app/project/SceneProjectIO.h"
#include "app/project/SceneProjectIODetail.h"

#include <nlohmann/json.hpp>

namespace SceneContentSerialization
{
    nlohmann::json SerializeObjects(const Scene& scene, const std::string& projectRoot)
    {
        return SceneProjectIODetail::SerializeObjects(scene, projectRoot);
    }

    bool DeserializeObjects(
        Scene& scene,
        const nlohmann::json& objects,
        int formatVersion,
        const std::string& projectRoot,
        std::string& outError)
    {
        return SceneProjectIODetail::DeserializeObjects(
            scene,
            objects,
            formatVersion,
            projectRoot,
            outError);
    }

    nlohmann::json SerializeSpawnCounters(const Scene& scene)
    {
        return SceneProjectIODetail::SerializeSpawnCounters(scene);
    }

    void DeserializeSpawnCounters(Scene& scene, const nlohmann::json& spawnCounters)
    {
        SceneProjectIODetail::DeserializeSpawnCounters(scene, spawnCounters);
    }

    nlohmann::json SerializeSelection(const Scene& scene)
    {
        return SceneProjectIODetail::SerializeSelection(scene);
    }

    void DeserializeSelection(Scene& scene, const nlohmann::json& selection)
    {
        SceneProjectIODetail::DeserializeSelection(scene, selection);
    }

    nlohmann::json SerializeSceneContent(const Scene& scene, const std::string& projectRoot)
    {
        return SceneProjectIODetail::SerializeSceneContent(scene, projectRoot);
    }

    bool DeserializeSceneContent(
        Scene& scene,
        const nlohmann::json& content,
        int formatVersion,
        const std::string& projectRoot,
        std::string& outError,
        ImportedMeshReusePool* meshReusePool,
        bool showProgress)
    {
        return SceneProjectIODetail::DeserializeSceneContent(
            scene,
            content,
            formatVersion,
            projectRoot,
            outError,
            meshReusePool,
            showProgress);
    }
}

SceneDocument SceneDocument::Capture(const Scene& scene, const std::string& projectRoot)
{
    SceneDocument document;
    document.m_content = SceneProjectIODetail::SerializeSceneContent(scene, projectRoot);
    return document;
}

bool SceneDocument::Apply(Scene& scene, const std::string& projectRoot, std::string& outError, bool forUndoRedo) const
{
    return SceneProjectIODetail::DeserializeSceneContent(
        scene,
        m_content,
        SceneProjectIO::CurrentFormatVersion,
        projectRoot,
        outError,
        nullptr,
        !forUndoRedo);
}

bool SceneDocument::IsSameAs(const SceneDocument& other) const
{
    return m_content == other.m_content;
}
