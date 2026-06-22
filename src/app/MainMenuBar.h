#pragma once

struct GLFWwindow;

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
        GLFWwindow* window,
        const EditorPanelVisibility& panels);
};
