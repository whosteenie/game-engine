#include "app/inspector/InspectorMultiEdit.h"
#include "app/undo/UndoCommand.h"
#include "app/editor/EditorMouseWrapping.h"
#include "app/editor/EditorWidgets.h"

#include <imgui.h>

#include <cmath>
#include <cstdio>

namespace
{
    constexpr float kTransformRowLabelWidth = 68.0f;

    const char* const kAxisLabels[] = {"X", "Y", "Z"};
    const ImVec4 kAxisColors[] = {
        ImVec4(0.86f, 0.33f, 0.33f, 1.0f),
        ImVec4(0.52f, 0.78f, 0.40f, 1.0f),
        ImVec4(0.42f, 0.58f, 0.92f, 1.0f),
    };

    bool ApproximatelyEqual(float left, float right, float epsilon)
    {
        return std::fabs(left - right) <= epsilon;
    }

    bool DrawMultiTransformRowLabel(
        const char* label,
        MultiVec3& field,
        const glm::vec3& resetValue)
    {
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgActive));
        ImGui::Selectable(label, false, ImGuiSelectableFlags_None);

        bool changed = false;
        if (ImGui::BeginPopupContextItem())
        {
            char menuLabel[64];
            std::snprintf(menuLabel, sizeof(menuLabel), "Reset %s", label);
            if (ImGui::MenuItem(menuLabel))
            {
                field.value = resetValue;
                field.ClearMixed();
                changed = true;
            }

            ImGui::EndPopup();
        }

        ImGui::PopStyleColor(3);
        return changed;
    }

    bool DrawMultiTransformAxisField(
        int axis,
        const char* label,
        MultiVec3& field,
        const glm::vec3& resetValue,
        float dragSpeed,
        const char* format,
        TransformEditContext* editContext)
    {
        ImGui::PushID(axis);
        ImGui::AlignTextToFramePadding();

        ImGui::PushStyleColor(ImGuiCol_Text, kAxisColors[axis]);
        ImGui::TextUnformatted(kAxisLabels[axis]);
        ImGui::PopStyleColor();

        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);

        bool changed = false;
        if (field.mixed[axis])
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            char placeholder[] = "---";
            ImGui::InputText("##mixed", placeholder, sizeof(placeholder), ImGuiInputTextFlags_ReadOnly);
            ImGui::PopStyleColor();

            if (ImGui::IsItemActivated())
            {
                field.mixed[axis] = false;
            }

            if (editContext != nullptr)
            {
                HandleTransformFieldEditEvents(*editContext);
            }
        }
        else
        {
            field.value[axis] = EditorWidgets::SanitizeSignedZero(field.value[axis]);
            changed = ImGui::DragFloat("##value", &field.value[axis], dragSpeed, 0.0f, 0.0f, format);
            if (changed)
            {
                field.value[axis] = EditorWidgets::SanitizeSignedZero(field.value[axis]);
            }
            EditorMouseWrapping::MarkCurrentItemForMouseWrap();
            if (editContext != nullptr)
            {
                HandleTransformFieldEditEvents(*editContext);
            }

            if (ImGui::BeginPopupContextItem())
            {
                char menuLabel[64];
                std::snprintf(menuLabel, sizeof(menuLabel), "Reset %s %s", kAxisLabels[axis], label);
                if (ImGui::MenuItem(menuLabel))
                {
                    field.value[axis] = resetValue[axis];
                    changed = true;
                }

                ImGui::EndPopup();
            }
        }

        ImGui::PopID();
        return changed;
    }
}

MultiBool MultiBool::Collect(const std::vector<bool>& values)
{
    MultiBool field;
    if (values.empty())
    {
        return field;
    }

    field.value = values.front();
    for (std::size_t index = 1; index < values.size(); ++index)
    {
        if (values[index] != field.value)
        {
            field.hasMixed = true;
            break;
        }
    }

    return field;
}

MultiBool MultiBool::Collect(const bool* values, std::size_t count)
{
    MultiBool field;
    if (count == 0)
    {
        return field;
    }

    field.value = values[0];
    for (std::size_t index = 1; index < count; ++index)
    {
        if (values[index] != field.value)
        {
            field.hasMixed = true;
            break;
        }
    }

    return field;
}

MultiVec3 MultiVec3::Collect(const glm::vec3* values, std::size_t count, float epsilon)
{
    MultiVec3 field;
    if (count == 0)
    {
        return field;
    }

    field.value = values[0];
    for (int axis = 0; axis < 3; ++axis)
    {
        for (std::size_t index = 1; index < count; ++index)
        {
            if (!ApproximatelyEqual(values[index][axis], field.value[axis], epsilon))
            {
                field.mixed[axis] = true;
                break;
            }
        }
    }

    return field;
}

bool MultiVec3::HasAnyMixed() const
{
    return mixed[0] || mixed[1] || mixed[2];
}

void MultiVec3::ClearMixed()
{
    mixed[0] = false;
    mixed[1] = false;
    mixed[2] = false;
}

bool DrawMultiCheckbox(const char* label, MultiBool& field)
{
    if (field.hasMixed)
    {
        ImGui::PushID(label);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine();

        char placeholder[] = "---";
        ImGui::SetNextItemWidth(ImGui::GetFrameHeight());
        ImGui::InputText("##mixed", placeholder, sizeof(placeholder), ImGuiInputTextFlags_ReadOnly);
        if (ImGui::IsItemActivated())
        {
            field.hasMixed = false;
        }

        ImGui::SameLine();
        bool checkboxValue = field.value;
        if (ImGui::Checkbox("##value", &checkboxValue))
        {
            field.value = checkboxValue;
            field.hasMixed = false;
            ImGui::PopID();
            return true;
        }

        ImGui::PopID();
        return false;
    }

    bool checkboxValue = field.value;
    if (ImGui::Checkbox(label, &checkboxValue))
    {
        field.value = checkboxValue;
        return true;
    }

    return false;
}

bool DrawMultiVec3Row(
    const char* label,
    MultiVec3& field,
    const glm::vec3& resetValue,
    float dragSpeed,
    const char* format,
    TransformEditContext* editContext)
{
    ImGui::PushID(label);
    ImGui::TableNextRow();

    ImGui::TableSetColumnIndex(0);
    bool changed = DrawMultiTransformRowLabel(label, field, resetValue);

    for (int axis = 0; axis < 3; ++axis)
    {
        ImGui::TableSetColumnIndex(axis + 1);
        changed |= DrawMultiTransformAxisField(
            axis,
            label,
            field,
            resetValue,
            dragSpeed,
            format,
            editContext);
    }

    ImGui::PopID();
    return changed;
}
