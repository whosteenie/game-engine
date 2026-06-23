#include "app/core/PlayModeController.h"

#include "app/core/PhysicsWorld.h"
#include "app/scene/Scene.h"
#include "engine/scene/Transform.h"

#include <exception>
#include <utility>

PlayModeController::PlayModeController() = default;

PlayModeController::~PlayModeController() = default;

bool PlayModeController::TogglePlayStop(Scene& editScene, const std::string& projectRoot)
{
    m_lastError.clear();

    if (m_state == PlayModeState::Edit)
    {
        return EnterPlay(editScene, projectRoot);
    }

    return Stop(editScene, projectRoot);
}

bool PlayModeController::TogglePause()
{
    m_lastError.clear();

    if (m_state == PlayModeState::Playing)
    {
        m_state = PlayModeState::Paused;
        return true;
    }

    if (m_state == PlayModeState::Paused)
    {
        m_state = PlayModeState::Playing;
        return true;
    }

    return false;
}

bool PlayModeController::ConsumeFocusGameViewRequest()
{
    const bool request = m_requestFocusGameView;
    m_requestFocusGameView = false;
    return request;
}

Scene* PlayModeController::GetRuntimeScene()
{
    return m_runtimeScene.get();
}

const Scene* PlayModeController::GetRuntimeScene() const
{
    return m_runtimeScene.get();
}

void PlayModeController::Simulate(double deltaTime)
{
    if (!IsSimulating() || m_runtimeScene == nullptr || m_physicsWorld == nullptr)
    {
        return;
    }

    m_physicsWorld->Step(*m_runtimeScene, static_cast<float>(deltaTime));
}

void PlayModeController::DebugNudgeRuntimeSelection(float deltaY)
{
    if (!IsActive() || m_runtimeScene == nullptr || !m_runtimeScene->HasSelection())
    {
        return;
    }

    const int objectIndex = m_runtimeScene->GetPrimarySelection();
    if (objectIndex < 0)
    {
        return;
    }

    Transform& transform = m_runtimeScene->GetObject(static_cast<std::size_t>(objectIndex)).GetTransform();
    transform.position.y += deltaY;
}

bool PlayModeController::EnterPlay(Scene& editScene, const std::string& /*projectRoot*/)
{
    if (m_state != PlayModeState::Edit)
    {
        return false;
    }

    try
    {
        m_runtimeScene = Scene::CloneForPlayMode(editScene);
        if (m_runtimeScene == nullptr)
        {
            m_lastError = "Failed to clone scene for play mode.";
            return false;
        }

        m_physicsWorld = std::make_unique<PhysicsWorld>();
        m_physicsWorld->BuildFromScene(*m_runtimeScene);
    }
    catch (const std::exception& exception)
    {
        m_lastError = std::string("Play mode failed: ") + exception.what();
        m_physicsWorld.reset();
        m_runtimeScene.reset();
        return false;
    }
    catch (...)
    {
        m_lastError = "Play mode failed with unknown exception.";
        m_physicsWorld.reset();
        m_runtimeScene.reset();
        return false;
    }

    m_state = PlayModeState::Playing;
    m_requestFocusGameView = true;
    return true;
}

bool PlayModeController::Stop(Scene& /*editScene*/, const std::string& /*projectRoot*/)
{
    if (m_state == PlayModeState::Edit)
    {
        return false;
    }

    if (m_physicsWorld != nullptr)
    {
        m_physicsWorld->Shutdown();
        m_physicsWorld.reset();
    }

    m_runtimeScene.reset();
    m_state = PlayModeState::Edit;
    return true;
}
