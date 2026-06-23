#pragma once

#include "app/project/ProjectEditorState.h"

#include <nlohmann/json_fwd.hpp>

#include <string>

class Scene;

struct SceneProjectIO
{
    static constexpr int CurrentFormatVersion = 3;

    static bool Save(
        const Scene& scene,
        const ProjectEditorState& editorState,
        const std::string& projectRoot,
        const std::string& projectFilePath,
        std::string& outError);

    static bool Load(
        Scene& scene,
        ProjectEditorState& editorState,
        const std::string& projectRoot,
        const std::string& projectFilePath,
        std::string& outError);

    static bool SaveEditorLayout(const std::string& projectRoot);
    static bool LoadEditorLayout(const std::string& projectRoot);
    static bool DeleteEditorLayout(const std::string& projectRoot);

private:
    static nlohmann::json SerializeScene(
        const Scene& scene,
        const ProjectEditorState& editorState,
        const std::string& projectRoot);
    static bool DeserializeScene(
        Scene& scene,
        ProjectEditorState& editorState,
        const nlohmann::json& root,
        const std::string& projectRoot,
        std::string& outError);
};
