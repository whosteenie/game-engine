#pragma once

#include <glm/glm.hpp>

#include "engine/SceneObjectId.h"

#include <string>
#include <unordered_map>

struct ProjectEditorState
{
    glm::vec3 cameraPosition = glm::vec3(6.0f, 5.0f, 6.0f);
    float cameraYaw = -135.0f;
    float cameraPitch = -35.0f;

    bool showHierarchy = true;
    bool showInspector = true;
    bool showToolbar = true;
    bool showLighting = true;
    bool showProjectFiles = true;

    std::unordered_map<SceneObjectId, bool> hierarchyNodeOpenStates;

    std::string projectFilesBrowsedDirectory;
    std::string projectFilesSelectedPath;
    std::unordered_map<std::string, bool> projectFilesFolderOpenStates;

    static ProjectEditorState CreateDefault();
};
