#pragma once

#include "app/ProjectEditorState.h"
#include "app/UndoStack.h"

#include <functional>

struct GLFWwindow;

class EditorSettings;
class EditorClipboard;
class ProjectSession;
class Scene;

struct EditorPanelVisibility
{
    bool* hierarchy = nullptr;
    bool* inspector = nullptr;
    bool* toolbar = nullptr;
    bool* lighting = nullptr;
    bool* project = nullptr;
    bool* sceneView = nullptr;
};

using CaptureEditorStateFn = std::function<void(ProjectEditorState&)>;
using ApplyEditorStateFn = std::function<void(const ProjectEditorState&)>;

class MainMenuBar
{
public:
    void Draw(
        Scene& scene,
        ProjectSession& project,
        EditorSettings& settings,
        GLFWwindow* window,
        const EditorPanelVisibility& panels,
        ProjectEditorState& editorState,
        const CaptureEditorStateFn& captureEditorState,
        const ApplyEditorStateFn& applyEditorState,
        const std::function<void()>& requestClose,
        const std::function<void()>& requestNewProject,
        const std::function<void()>& requestResetLayout,
        UndoStack& undoStack,
        EditorClipboard& clipboard,
        bool allowUndoRedo = true);
};
