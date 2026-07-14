#pragma once

#include "app/project/ProjectEditorState.h"
#include "app/undo/UndoStack.h"

#include <functional>
#include <string>

struct GLFWwindow;

class EditorSettings;
class EditorClipboard;
class PlayModeController;
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
    bool* gameView = nullptr;
    bool* performance = nullptr;
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
        const std::function<void()>& alignSelectionToView,
        PlayModeController& playMode,
        UndoStack& undoStack,
        EditorClipboard& clipboard,
        bool allowUndoRedo = true);

private:
    bool m_showOpenProjectModal = false;
    std::string m_openProjectError;
    bool m_showSaveAsModal = false;
    bool m_focusSaveAsName = false;
    char m_saveAsProjectName[64] = {};
    char m_saveAsParentDirectory[512] = {};
    std::string m_saveAsError;
};
