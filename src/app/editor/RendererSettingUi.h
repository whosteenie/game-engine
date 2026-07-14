#pragma once

#include "app/editor/SettingRegistry.h"
#include "app/editor/TuningSectionState.h"
#include "app/scene/Scene.h"
#include "app/undo/UndoCommand.h"

#include <functional>
#include <stdexcept>

namespace RendererSettingUi
{
    inline const SettingRegistry::Descriptor& Require(const char* id)
    {
        const SettingRegistry::Descriptor* descriptor = SettingRegistry::FindById(id);
        if (descriptor == nullptr)
        {
            throw std::logic_error("Renderer setting is missing from SettingRegistry");
        }
        return *descriptor;
    }

    inline void MarkRendered(const char* id)
    {
        const SettingRegistry::Descriptor& descriptor = Require(id);
        TuningSectionState::MarkSearchTarget(id);
    }

    inline void HandleFieldEdit(const char* id, RendererEditContext& context)
    {
        if (Require(id).undoPolicy == SettingRegistry::UndoPolicy::Undoable)
        {
            HandleRendererFieldEditEvents(context);
        }
    }

    inline void ApplyChange(
        const char* id,
        RendererEditContext& context,
        Scene& scene,
        const char* commandName,
        const std::function<void(Scene&)>& mutate)
    {
        const SettingRegistry::Descriptor& descriptor = Require(id);
        if (descriptor.undoPolicy == SettingRegistry::UndoPolicy::Undoable && context.undoStack != nullptr)
        {
            PushRendererMutation(*context.undoStack, scene, commandName, mutate);
            return;
        }

        mutate(scene);
    }
}
