#include "app/scene/SceneEditingController.h"

#include "app/scene/Scene.h"
#include "app/scene/SceneEditor.h"
#include "app/scene/SceneEditorUpdateContext.h"

SceneEditingController::SceneEditingController()
    : m_editor(std::make_unique<SceneEditor>())
{
}

SceneEditingController::~SceneEditingController() = default;

SceneEditor& SceneEditingController::GetEditor()
{
    return *m_editor;
}

const SceneEditor& SceneEditingController::GetEditor() const
{
    return *m_editor;
}

void SceneEditingController::Update(Scene& scene, const SceneEditorUpdateContext& context)
{
    m_editor->Update(
        scene,
        context.camera,
        context.input,
        context.framebufferWidth,
        context.framebufferHeight,
        context.windowWidth,
        context.windowHeight,
        context.allowMouseInput,
        context.allowKeyboardInput,
        context.undoStack,
        context.projectRoot,
        context.viewport);
}

void SceneEditingController::HandleEscapeKey(Scene& scene)
{
    m_editor->HandleEscapeKey(scene);
}
