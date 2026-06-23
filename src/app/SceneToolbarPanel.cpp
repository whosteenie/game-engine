#include "app/SceneToolbarPanel.h"

#include "app/Scene.h"
#include "app/SceneEditor.h"

#include <imgui.h>

namespace
{
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
    const EditorViewportRect& sceneViewRect) const
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

    bool showGrid = scene.GetShowGrid();
    if (ImGui::Checkbox("Grid", &showGrid))
    {
        scene.SetShowGrid(showGrid);
    }

    ImGui::Separator();
    ImGui::TextUnformatted("LMB: select/deselect or drag gizmo.");
    ImGui::SameLine();
    ImGui::TextUnformatted("RMB + WASD/Q/E: fly camera. Shift: move faster.");

    ImGui::End();
}
