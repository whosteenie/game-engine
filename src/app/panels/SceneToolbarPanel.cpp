#include "app/panels/SceneToolbarPanel.h"

#include "app/scene/Scene.h"
#include "app/scene/SceneEditor.h"
#include "app/undo/UndoCommand.h"

#include <imgui.h>

namespace
{
    constexpr const char* kShadingModeLabels[] = {
        "Full Runtime",
        "Lit",
        "Unlit",
    };

    const char* ShadingModeTooltip(const SceneViewShadingMode mode)
    {
        switch (mode)
        {
        case SceneViewShadingMode::FullRuntime:
            return "Scene View uses the project's complete renderer tuning, including path tracing when enabled.";
        case SceneViewShadingMode::Lit:
            return "Scene View uses the raster lighting path while Game View remains fully representative.";
        case SceneViewShadingMode::Unlit:
            return "Scene View shows material base color without lighting. Game View remains fully representative.";
        default:
            return nullptr;
        }
    }

    bool DrawToolButton(const char* label, TransformTool tool, TransformTool activeTool, Scene& scene)
    {
        const bool isActive = tool == activeTool;
        if (isActive)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f, 0.55f, 0.85f, 1.0f));
        }

        const bool clicked = ImGui::Button(label);
        if (isActive)
        {
            ImGui::PopStyleColor();
        }

        if (clicked)
        {
            scene.GetSceneEditor().SetTool(tool);
        }

        return clicked;
    }

    bool DrawSpaceButton(const char* label, TransformSpace space, TransformSpace activeSpace, Scene& scene)
    {
        const bool isActive = space == activeSpace;
        if (isActive)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f, 0.55f, 0.85f, 1.0f));
        }

        const bool clicked = ImGui::Button(label);
        if (isActive)
        {
            ImGui::PopStyleColor();
        }

        if (clicked)
        {
            scene.GetSceneEditor().SetTransformSpace(space);
        }

        return clicked;
    }
}

void SceneToolbarPanel::Draw(
    Scene& scene,
    bool sceneViewVisible,
    const EditorViewportRect& sceneViewRect,
    UndoStack* undoStack) const
{
    if (!m_showPanel || !sceneViewVisible || !sceneViewRect.valid)
    {
        return;
    }

    constexpr float kAnchorMargin = 8.0f;
    ImGui::SetNextWindowPos(
        ImVec2(sceneViewRect.screenX + kAnchorMargin, sceneViewRect.screenY + kAnchorMargin),
        ImGuiCond_Always);

    if (!ImGui::Begin(
            "Toolbar",
            &m_showPanel,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking))
    {
        return;
    }

    SceneEditor& editor = scene.GetSceneEditor();
    const TransformTool activeTool = editor.GetTool();
    const TransformSpace activeSpace = editor.GetTransformSpace();

    DrawToolButton("Move (W)", TransformTool::Translate, activeTool, scene);
    ImGui::SameLine();
    DrawToolButton("Rotate (E)", TransformTool::Rotate, activeTool, scene);
    ImGui::SameLine();
    DrawToolButton("Scale (R)", TransformTool::Scale, activeTool, scene);

    if (activeTool != TransformTool::Scale)
    {
        ImGui::SameLine();
        DrawSpaceButton("Local (L)", TransformSpace::Local, activeSpace, scene);
        ImGui::SameLine();
        DrawSpaceButton("World (G)", TransformSpace::World, activeSpace, scene);
    }
    else
    {
        ImGui::SameLine();
        ImGui::TextUnformatted("Scale gizmo uses local axes.");
    }

    ImGui::Separator();

    int shadingMode = static_cast<int>(m_shadingMode);
    ImGui::TextUnformatted("Shading");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f);
    if (ImGui::Combo("##SceneViewShading", &shadingMode, kShadingModeLabels, IM_ARRAYSIZE(kShadingModeLabels)))
    {
        m_shadingMode = static_cast<SceneViewShadingMode>(shadingMode);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", ShadingModeTooltip(m_shadingMode));
    }

    ImGui::Separator();

    bool showGrid = scene.GetShowGrid();
    if (ImGui::Checkbox("Grid", &showGrid))
    {
        if (undoStack != nullptr)
        {
            PushSceneEditorViewMutation(*undoStack, scene, "Grid visibility", [&](Scene& target) {
                target.SetShowGrid(showGrid);
            });
        }
        else
        {
            scene.SetShowGrid(showGrid);
        }
    }

    ImGui::Separator();
    ImGui::TextUnformatted("LMB: select/deselect or drag gizmo.");
    ImGui::TextUnformatted("Move gizmo center + Alt: surface snap.");
    ImGui::TextUnformatted("Rotate gizmo + Ctrl: snap 15 degrees.");
    ImGui::TextUnformatted("RMB + WASD/Q/E: fly camera. Wheel: speed. Shift: move faster.");

    ImGui::End();
}
