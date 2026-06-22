#pragma once

#include <functional>

struct GLFWwindow;

class EditorSettings;
class ProjectSession;
class Scene;

struct EditorPanelVisibility
{
    bool* hierarchy = nullptr;
    bool* inspector = nullptr;
    bool* toolbar = nullptr;
    bool* lighting = nullptr;
    bool* project = nullptr;
};

class MainMenuBar
{
public:
    void Draw(
        Scene& scene,
        ProjectSession& project,
        EditorSettings& settings,
        GLFWwindow* window,
        const EditorPanelVisibility& panels,
        const std::function<void()>& requestClose,
        const std::function<void()>& requestNewProject);
};
