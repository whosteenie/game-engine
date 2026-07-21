#include "app/undo/UndoCommand.h"

#include "app/editor/EditorClipboard.h"
#include "app/editor/TuningSectionState.h"
#include "app/scene/document/Scene.h"
#include "app/project/SceneDocument.h"
#include "app/project/SceneProjectIODetail.h"
#include "app/project/SceneSubtreeArchive.h"
#include "engine/rendering/resources/Mesh.h"
#include "app/undo/UndoStack.h"
#include "engine/components/CameraComponent.h"
#include "engine/components/ColliderComponent.h"
#include "engine/scene/InspectorComponentOrder.h"
#include "engine/components/LightComponent.h"
#include "engine/rendering/resources/Material.h"
#include "engine/components/RigidBodyComponent.h"
#include "engine/scene/SceneObject.h"
#include "engine/scene/SceneObjectId.h"

#include "engine/scene/SceneHierarchy.h"

nlohmann::json CaptureRendererSettings(const Scene& scene)
{
    return SceneProjectIODetail::SerializeRenderer(scene);
}

namespace
{
    nlohmann::json BuildRendererSettingsDelta(
        const nlohmann::json& before,
        const nlohmann::json& after,
        const nlohmann::json& values)
    {
        if (before == after)
        {
            return nlohmann::json::object();
        }

        if (!before.is_object() || !after.is_object() || !values.is_object())
        {
            return values;
        }

        nlohmann::json delta = nlohmann::json::object();
        for (const auto& [key, afterValue] : after.items())
        {
            const nlohmann::json beforeValue =
                before.contains(key) ? before.at(key) : nlohmann::json();
            const nlohmann::json value =
                values.contains(key) ? values.at(key) : nlohmann::json();
            nlohmann::json childDelta = BuildRendererSettingsDelta(beforeValue, afterValue, value);
            if (!childDelta.is_object() || !childDelta.empty())
            {
                delta[key] = std::move(childDelta);
            }
        }

        for (const auto& [key, beforeValue] : before.items())
        {
            if (after.contains(key))
            {
                continue;
            }

            const nlohmann::json value =
                values.contains(key) ? values.at(key) : nlohmann::json();
            delta[key] = value;
        }

        return delta;
    }

    void ApplyRendererSettingsDelta(Scene& scene, const nlohmann::json& delta)
    {
        SceneProjectIODetail::ApplyRendererSettingsDelta(scene, delta);
    }
}

bool AreRendererSettingsEqual(const nlohmann::json& left, const nlohmann::json& right)
{
    return left == right;
}

void ApplyRendererSettings(Scene& scene, const nlohmann::json& settings)
{
    SceneProjectIODetail::ApplyRendererSettingsDelta(scene, settings);
}

RendererSettingsCommand::RendererSettingsCommand(
    nlohmann::json before,
    nlohmann::json after,
    std::string name)
    : m_before(BuildRendererSettingsDelta(before, after, before)),
      m_after(BuildRendererSettingsDelta(before, after, after)),
      m_name(std::move(name))
{
}

void RendererSettingsCommand::Undo(UndoContext& context)
{
    ApplyRendererSettingsDelta(context.scene, m_before);
}

void RendererSettingsCommand::Redo(UndoContext& context)
{
    ApplyRendererSettingsDelta(context.scene, m_after);
}

const char* RendererSettingsCommand::GetName() const
{
    return m_name.c_str();
}

bool RendererSettingsCommand::TryMerge(const IUndoCommand& next)
{
    (void)next;
    return false;
}

void PushRendererSettings(
    UndoStack& undoStack,
    nlohmann::json before,
    nlohmann::json after,
    const std::string& commandName)
{
    if (AreRendererSettingsEqual(before, after))
    {
        return;
    }

    undoStack.Push(std::make_unique<RendererSettingsCommand>(
        std::move(before),
        std::move(after),
        commandName));
}

void PushRendererMutation(
    UndoStack& undoStack,
    Scene& scene,
    const std::string& commandName,
    const std::function<void(Scene&)>& mutate)
{
    if (!mutate)
    {
        return;
    }

    nlohmann::json before = CaptureRendererSettings(scene);
    mutate(scene);
    nlohmann::json after = CaptureRendererSettings(scene);
    PushRendererSettings(undoStack, std::move(before), std::move(after), commandName);
}

void BeginRendererEditFrame(RendererEditContext& context)
{
    if (context.scene == nullptr)
    {
        context.hasFrameBefore = false;
        context.frameBefore = nlohmann::json();
        return;
    }

    context.frameBefore = CaptureRendererSettings(*context.scene);
    context.hasFrameBefore = true;
}

void HandleRendererFieldEditEvents(RendererEditContext& context)
{
    TuningSectionState::MarkCurrentItemIfSearchTarget();

    if (!TuningSectionState::CurrentItemIsUndoable())
    {
        return;
    }

    if (context.undoStack == nullptr || context.scene == nullptr)
    {
        return;
    }

    if (ImGui::IsItemActivated() && !context.sessionOpen)
    {
        context.pendingBefore = context.hasFrameBefore
            ? context.frameBefore
            : CaptureRendererSettings(*context.scene);
        context.sessionOpen = true;
    }

    if (ImGui::IsItemDeactivatedAfterEdit() && context.sessionOpen)
    {
        nlohmann::json after = CaptureRendererSettings(*context.scene);
        PushRendererSettings(
            *context.undoStack,
            std::move(context.pendingBefore),
            std::move(after),
            context.commandName);
        context.sessionOpen = false;
    }
}
