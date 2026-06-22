#pragma once

#include <functional>
#include <string>

class EditorSettings;
class ProjectSession;
class Scene;

class ProjectChooser
{
public:
    using RequestCloseCallback = std::function<void()>;
    using NewProjectRequestedCallback = std::function<bool()>;

    bool Draw(
        ProjectSession& project,
        Scene& scene,
        EditorSettings& settings,
        const RequestCloseCallback& requestClose);

    void OpenNewProjectForm();
    bool IsBlockingEditor() const;

private:
    bool DrawStartupScreen(
        ProjectSession& project,
        Scene& scene,
        EditorSettings& settings,
        const RequestCloseCallback& requestClose);

    bool DrawNewProjectForm(ProjectSession& project, Scene& scene, EditorSettings& settings);

    bool TryOpenProject(
        ProjectSession& project,
        Scene& scene,
        EditorSettings& settings,
        const std::string& projectFilePath,
        std::string& outError);

    bool m_showNewProjectForm = false;
    bool m_startupMode = true;
    char m_newProjectName[64] = "My Project";
    char m_newProjectDirectory[512] = {};
    std::string m_errorMessage;
};
