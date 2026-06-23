#pragma once

#include "app/ProjectEditorState.h"

#include <functional>
#include <string>

class EditorSettings;
class EditorClipboard;
class ProjectSession;
class Scene;
class UndoStack;

class ProjectChooser
{
public:
    using RequestCloseCallback = std::function<void()>;
    using ApplyEditorStateFn = std::function<void(const ProjectEditorState&)>;

    bool Draw(
        ProjectSession& project,
        Scene& scene,
        EditorSettings& settings,
        ProjectEditorState& editorState,
        const ApplyEditorStateFn& applyEditorState,
        const RequestCloseCallback& requestClose,
        UndoStack& undoStack,
        EditorClipboard& clipboard);

    void OpenNewProjectForm(EditorSettings& settings);
    bool IsBlockingEditor() const;

private:
    bool DrawStartupScreen(
        ProjectSession& project,
        Scene& scene,
        EditorSettings& settings,
        ProjectEditorState& editorState,
        const ApplyEditorStateFn& applyEditorState,
        const RequestCloseCallback& requestClose,
        UndoStack& undoStack,
        EditorClipboard& clipboard);

    bool DrawNewProjectForm(
        ProjectSession& project,
        Scene& scene,
        EditorSettings& settings,
        const ApplyEditorStateFn& applyEditorState,
        UndoStack& undoStack,
        EditorClipboard& clipboard);

    bool TryOpenProject(
        ProjectSession& project,
        Scene& scene,
        EditorSettings& settings,
        ProjectEditorState& editorState,
        const std::string& projectFilePath,
        const ApplyEditorStateFn& applyEditorState,
        UndoStack& undoStack,
        EditorClipboard& clipboard,
        std::string& outError);

    bool m_showNewProjectForm = false;
    bool m_startupMode = true;
    char m_newProjectName[64] = "My Project";
    char m_newProjectDirectory[512] = {};
    std::string m_errorMessage;
};
