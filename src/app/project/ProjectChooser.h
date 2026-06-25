#pragma once

#include "app/project/ProjectEditorState.h"

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
    using FinalizeEditorOpenFn = std::function<void()>;

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
    void SetErrorMessage(const std::string& message) { m_errorMessage = message; }
    void ReturnToStartupWithError(ProjectSession& project, Scene& scene, const std::string& message);

    bool OpenProjectAtPath(
        ProjectSession& project,
        Scene& scene,
        EditorSettings& settings,
        ProjectEditorState& editorState,
        const std::string& projectFilePath,
        const ApplyEditorStateFn& applyEditorState,
        UndoStack& undoStack,
        EditorClipboard& clipboard,
        const FinalizeEditorOpenFn& finalizeEditorOpen,
        std::string& outError);

    bool ProcessPendingProjectOpen(
        ProjectSession& project,
        Scene& scene,
        EditorSettings& settings,
        ProjectEditorState& editorState,
        const ApplyEditorStateFn& applyEditorState,
        UndoStack& undoStack,
        EditorClipboard& clipboard,
        const FinalizeEditorOpenFn& finalizeEditorOpen,
        std::string& outError);

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
    std::string m_pendingProjectPath;
};
