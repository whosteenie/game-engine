#pragma once

// Scene-mutating editor UI must use these helpers (or call Handle*FieldEditEvents
// after ImGui widgets with the same activate/deactivate undo session pattern).
//
// Apply callbacks receive the current widget value after ImGui has updated it.
// Do not capture locals by value in call-site lambdas — use the callback parameter.

#include "app/scene/Scene.h"
#include "app/undo/UndoCommand.h"

#include <imgui.h>

#include <functional>

inline void ApplyRendererChange(
    RendererEditContext& editContext,
    Scene& scene,
    const char* commandName,
    const std::function<void(Scene&)>& mutate)
{
    if (editContext.undoStack != nullptr)
    {
        PushRendererMutation(*editContext.undoStack, scene, commandName, mutate);
        return;
    }

    mutate(scene);
}

inline bool UndoableRendererSliderFloat(
    const char* label,
    float* value,
    const float min,
    const float max,
    const char* format,
    RendererEditContext& context,
    const std::function<void(Scene&, float)>& apply)
{
    const bool changed = ImGui::SliderFloat(label, value, min, max, format);
    if (changed && context.scene != nullptr)
    {
        apply(*context.scene, *value);
    }

    HandleRendererFieldEditEvents(context);
    return changed;
}

inline bool UndoableRendererSliderInt(
    const char* label,
    int* value,
    const int min,
    const int max,
    RendererEditContext& context,
    const std::function<void(Scene&, int)>& apply)
{
    const bool changed = ImGui::SliderInt(label, value, min, max);
    if (changed && context.scene != nullptr)
    {
        apply(*context.scene, *value);
    }

    HandleRendererFieldEditEvents(context);
    return changed;
}

inline bool UndoableRendererCheckbox(
    const char* label,
    bool* value,
    RendererEditContext& context,
    const std::function<void(Scene&, bool)>& apply)
{
    const bool changed = ImGui::Checkbox(label, value);
    if (changed && context.scene != nullptr)
    {
        ApplyRendererChange(context, *context.scene, label, [&](Scene& scene) {
            apply(scene, *value);
        });
    }

    return changed;
}

template<typename T>
inline bool UndoableSliderFloat(
    const char* label,
    float* value,
    const float min,
    const float max,
    const char* format,
    PropertyEditContext<T>& context,
    const std::function<std::unordered_map<SceneObjectId, T>(const Scene&, const std::vector<int>&)>& capture,
    const std::function<bool(const std::unordered_map<SceneObjectId, T>&, const std::unordered_map<SceneObjectId, T>&)>&
        equals,
    const std::function<void(Scene&, SceneObjectId, const T&)>& applyProperty,
    const std::function<void(Scene&)>& apply)
{
    const bool changed = ImGui::SliderFloat(label, value, min, max, format);
    if (changed && context.scene != nullptr)
    {
        apply(*context.scene);
    }

    HandlePropertyFieldEditEvents<T>(context, capture, equals, applyProperty);
    return changed;
}
