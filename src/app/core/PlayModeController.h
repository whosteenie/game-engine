#pragma once

#include <memory>
#include <string>

class Scene;
class PhysicsWorld;
class SceneEditor;

enum class PlayModeState
{
    Edit = 0,
    Playing,
    Paused
};

class PlayModeController
{
public:
    PlayModeController();
    ~PlayModeController();

    PlayModeState GetState() const { return m_state; }
    bool IsActive() const { return m_state != PlayModeState::Edit; }
    bool IsSimulating() const { return m_state == PlayModeState::Playing; }
    bool IsStartPaused() const { return m_startPaused; }

    const std::string& GetLastError() const { return m_lastError; }

    Scene* GetRuntimeScene();
    const Scene* GetRuntimeScene() const;

    void SetSceneEditor(SceneEditor& sceneEditor);

    bool TogglePlayStop(Scene& editScene, const std::string& projectRoot);
    bool TogglePause();

    bool ConsumeFocusGameViewRequest();

    void Simulate(double deltaTime);
    void StepOnce();
    void NotifyRuntimeSceneMutated();

    // Phase 1 debug helper — nudges the runtime selection so isolation can be verified without Game View.
    void DebugNudgeRuntimeSelection(float deltaY);

private:
    bool EnterPlay(Scene& editScene, const std::string& projectRoot);
    bool Stop(Scene& editScene, const std::string& projectRoot);
    void ForceStop();

    PlayModeState m_state = PlayModeState::Edit;
    std::unique_ptr<Scene> m_runtimeScene;
    std::unique_ptr<PhysicsWorld> m_physicsWorld;
    std::string m_lastError;
    bool m_requestFocusGameView = false;
    bool m_physicsRebuildPending = false;
    bool m_startPaused = false;
    SceneEditor* m_sceneEditor = nullptr;
};
