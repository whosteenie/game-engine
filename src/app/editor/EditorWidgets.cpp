#include "app/editor/EditorWidgets.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdarg>
#include <cstring>

namespace EditorWidgets
{
    namespace
    {
        // Matches imgui_widgets.cpp (not exported).
        constexpr float kDragMouseThresholdFactor = 0.50f;

        bool TempInputIsClampEnabled(ImGuiSliderFlags flags, ImGuiDataType dataType, const void* pMin, const void* pMax)
        {
            if ((flags & ImGuiSliderFlags_ClampOnInput) && (pMin != nullptr || pMax != nullptr))
            {
                const int clampRangeDir =
                    (pMin != nullptr && pMax != nullptr) ? ImGui::DataTypeCompare(dataType, pMin, pMax) : 0;
                if (pMin == nullptr || pMax == nullptr || clampRangeDir < 0)
                {
                    return true;
                }
                if (clampRangeDir == 0)
                {
                    return ImGui::DataTypeIsZero(dataType, pMin)
                        ? ((flags & ImGuiSliderFlags_ClampZeroRange) != 0)
                        : true;
                }
            }
            return false;
        }

        // Same as ImGui::TempInputScalar but with numeric char filters so letters never enter.
        bool TempInputScalarFiltered(
            const ImRect& bb,
            ImGuiID id,
            const char* label,
            ImGuiDataType dataType,
            void* pData,
            const char* format,
            const void* pClampMin,
            const void* pClampMax,
            ImGuiInputTextFlags charFilterFlags)
        {
            ImGuiContext& g = *GImGui;
            const ImGuiDataTypeInfo* typeInfo = ImGui::DataTypeGetInfo(dataType);
            char fmtBuf[32];
            char dataBuf[32];
            format = ImParseFormatTrimDecorations(format, fmtBuf, IM_ARRAYSIZE(fmtBuf));
            if (format[0] == 0)
            {
                format = typeInfo->PrintFmt;
            }
            ImGui::DataTypeFormatString(dataBuf, IM_ARRAYSIZE(dataBuf), dataType, pData, format);
            ImStrTrimBlanks(dataBuf);

            ImGuiInputTextFlags flags = ImGuiInputTextFlags_AutoSelectAll
                | static_cast<ImGuiInputTextFlags>(ImGuiInputTextFlags_LocalizeDecimalPoint)
                | charFilterFlags;
            g.LastItemData.ItemFlags |= ImGuiItemFlags_NoMarkEdited;
            if (!ImGui::TempInputText(bb, id, label, dataBuf, IM_ARRAYSIZE(dataBuf), flags))
            {
                return false;
            }

            const size_t dataTypeSize = typeInfo->Size;
            ImGuiDataTypeStorage dataBackup;
            std::memcpy(&dataBackup, pData, dataTypeSize);

            ImGui::DataTypeApplyFromText(dataBuf, dataType, pData, format, nullptr);
            if (pClampMin || pClampMax)
            {
                if (pClampMin && pClampMax && ImGui::DataTypeCompare(dataType, pClampMin, pClampMax) > 0)
                {
                    ImSwap(pClampMin, pClampMax);
                }
                ImGui::DataTypeClamp(dataType, pData, pClampMin, pClampMax);
            }

            g.LastItemData.ItemFlags &= ~ImGuiItemFlags_NoMarkEdited;
            const bool valueChanged = std::memcmp(&dataBackup, pData, dataTypeSize) != 0;
            if (valueChanged)
            {
                ImGui::MarkItemEdited(id);
            }
            return valueChanged;
        }

        bool DragScalarFiltered(
            const char* label,
            ImGuiDataType dataType,
            void* pData,
            float speed,
            const void* pMin,
            const void* pMax,
            const char* format,
            ImGuiSliderFlags flags,
            ImGuiInputTextFlags charFilterFlags)
        {
            ImGuiWindow* window = ImGui::GetCurrentWindow();
            if (window->SkipItems)
            {
                return false;
            }

            ImGuiContext& g = *GImGui;
            const ImGuiStyle& style = g.Style;
            const ImGuiID id = window->GetID(label);
            const float width = ImGui::CalcItemWidth();

            const char* labelEnd = ImGui::FindRenderedTextEnd(label);
            const ImVec2 labelSize = ImGui::CalcTextSize(label, labelEnd, false);
            const ImRect frameBb(
                window->DC.CursorPos,
                window->DC.CursorPos + ImVec2(width, labelSize.y + style.FramePadding.y * 2.0f));
            const ImRect totalBb(
                frameBb.Min,
                frameBb.Max
                    + ImVec2(labelSize.x > 0.0f ? style.ItemInnerSpacing.x + labelSize.x : 0.0f, 0.0f));

            const bool tempInputAllowed = (flags & ImGuiSliderFlags_NoInput) == 0;
            ImGui::ItemSize(totalBb, style.FramePadding.y);
            if (!ImGui::ItemAdd(
                    totalBb,
                    id,
                    &frameBb,
                    tempInputAllowed ? ImGuiItemFlags_Inputable : 0))
            {
                return false;
            }

            if (format == nullptr)
            {
                format = ImGui::DataTypeGetInfo(dataType)->PrintFmt;
            }

            const bool hovered = ImGui::ItemHoverable(frameBb, id, g.LastItemData.ItemFlags);
            bool tempInputIsActive = tempInputAllowed && ImGui::TempInputIsActive(id);
            if (!tempInputIsActive)
            {
                const bool clicked = hovered && ImGui::IsMouseClicked(0, ImGuiInputFlags_None, id);
                const bool doubleClicked =
                    hovered && g.IO.MouseClickedCount[0] == 2 && ImGui::TestKeyOwner(ImGuiKey_MouseLeft, id);
                const bool makeActive = clicked || doubleClicked || g.NavActivateId == id;
                if (makeActive && (clicked || doubleClicked))
                {
                    ImGui::SetKeyOwner(ImGuiKey_MouseLeft, id);
                }
                if (makeActive && tempInputAllowed)
                {
                    if ((clicked && g.IO.KeyCtrl) || doubleClicked
                        || (g.NavActivateId == id
                            && (g.NavActivateFlags & ImGuiActivateFlags_PreferInput)))
                    {
                        tempInputIsActive = true;
                    }
                }

                if (g.IO.ConfigDragClickToInputText && tempInputAllowed && !tempInputIsActive)
                {
                    if (g.ActiveId == id && hovered && g.IO.MouseReleased[0]
                        && !ImGui::IsMouseDragPastThreshold(
                            0,
                            g.IO.MouseDragThreshold * kDragMouseThresholdFactor))
                    {
                        g.NavActivateId = id;
                        g.NavActivateFlags = ImGuiActivateFlags_PreferInput;
                        tempInputIsActive = true;
                    }
                }

                if (makeActive)
                {
                    std::memcpy(
                        &g.ActiveIdValueOnActivation,
                        pData,
                        ImGui::DataTypeGetInfo(dataType)->Size);
                }

                if (makeActive && !tempInputIsActive)
                {
                    ImGui::SetActiveID(id, window);
                    ImGui::SetFocusID(id, window);
                    ImGui::FocusWindow(window);
                    g.ActiveIdUsingNavDirMask = (1 << ImGuiDir_Left) | (1 << ImGuiDir_Right);
                }
            }

            if (tempInputIsActive)
            {
                const bool clampEnabled = TempInputIsClampEnabled(flags, dataType, pMin, pMax);
                return TempInputScalarFiltered(
                    frameBb,
                    id,
                    label,
                    dataType,
                    pData,
                    format,
                    clampEnabled ? pMin : nullptr,
                    clampEnabled ? pMax : nullptr,
                    charFilterFlags);
            }

            const ImU32 frameCol = ImGui::GetColorU32(
                g.ActiveId == id ? ImGuiCol_FrameBgActive
                                 : (hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg));
            ImGui::RenderNavCursor(frameBb, id);
            ImGui::RenderFrame(frameBb.Min, frameBb.Max, frameCol, false, style.FrameRounding);
            ImGui::RenderFrameBorder(frameBb.Min, frameBb.Max, g.Style.FrameRounding);

            const bool valueChanged =
                ImGui::DragBehavior(id, dataType, pData, speed, pMin, pMax, format, flags);
            if (valueChanged)
            {
                ImGui::MarkItemEdited(id);
            }

            char valueBuf[64];
            const char* valueBufEnd = valueBuf
                + ImGui::DataTypeFormatString(valueBuf, IM_ARRAYSIZE(valueBuf), dataType, pData, format);
            ImGui::RenderTextClipped(
                frameBb.Min,
                frameBb.Max,
                valueBuf,
                valueBufEnd,
                nullptr,
                ImVec2(0.5f, 0.5f));

            if (labelSize.x > 0.0f)
            {
                ImGui::RenderText(
                    ImVec2(
                        frameBb.Max.x + style.ItemInnerSpacing.x,
                        frameBb.Min.y + style.FramePadding.y),
                    label,
                    labelEnd,
                    false);
            }

            return valueChanged;
        }
    }

    ImVec4 ErrorTextColor()
    {
        return ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
    }

    void DrawErrorText(const std::string& message)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ErrorTextColor());
        ImGui::TextWrapped("%s", message.c_str());
        ImGui::PopStyleColor();
    }

    void TextColoredError(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        ImGui::TextColoredV(ErrorTextColor(), fmt, args);
        va_end(args);
    }

    bool DragFloat(
        const char* label,
        float* value,
        float speed,
        float min,
        float max,
        const char* format,
        ImGuiSliderFlags flags)
    {
        SanitizeSignedZero(*value);
        const bool changed = DragScalarFiltered(
            label,
            ImGuiDataType_Float,
            value,
            speed,
            &min,
            &max,
            format,
            flags,
            ImGuiInputTextFlags_CharsScientific);
        if (changed)
        {
            SanitizeSignedZero(*value);
        }
        return changed;
    }

    bool DragFloat3(
        const char* label,
        float value[3],
        float speed,
        float min,
        float max,
        const char* format,
        ImGuiSliderFlags flags)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems)
        {
            return false;
        }

        ImGuiContext& g = *GImGui;
        bool changed = false;
        ImGui::BeginGroup();
        ImGui::PushID(label);
        ImGui::PushMultiItemsWidths(3, ImGui::CalcItemWidth());
        for (int component = 0; component < 3; ++component)
        {
            ImGui::PushID(component);
            if (component > 0)
            {
                ImGui::SameLine(0.0f, g.Style.ItemInnerSpacing.x);
            }
            changed |= DragFloat("##v", &value[component], speed, min, max, format, flags);
            ImGui::PopID();
            ImGui::PopItemWidth();
        }
        ImGui::PopID();

        const char* labelEnd = ImGui::FindRenderedTextEnd(label);
        if (label != labelEnd)
        {
            ImGui::SameLine(0.0f, g.Style.ItemInnerSpacing.x);
            ImGui::TextEx(label, labelEnd);
        }
        ImGui::EndGroup();
        return changed;
    }

    bool DragInt(
        const char* label,
        int* value,
        float speed,
        int min,
        int max,
        const char* format,
        ImGuiSliderFlags flags)
    {
        return DragScalarFiltered(
            label,
            ImGuiDataType_S32,
            value,
            speed,
            &min,
            &max,
            format,
            flags,
            ImGuiInputTextFlags_CharsDecimal);
    }

    bool ColorEditVec3(const char* label, glm::vec3& value)
    {
        SanitizeSignedZero(value);
        const bool changed = ImGui::ColorEdit3(label, &value.x);
        if (changed)
        {
            SanitizeSignedZero(value);
        }
        return changed;
    }

    bool SliderVec3(const char* label, glm::vec3& value, float min, float max)
    {
        SanitizeSignedZero(value);
        const bool changed = ImGui::SliderFloat3(label, &value.x, min, max);
        if (changed)
        {
            SanitizeSignedZero(value);
        }
        return changed;
    }

    TextWrapScope::TextWrapScope()
    {
        const float wrapWidth = ImGui::GetCursorPos().x + ImGui::GetContentRegionAvail().x;
        if (wrapWidth > 0.0f)
        {
            ImGui::PushTextWrapPos(wrapWidth);
            m_active = true;
        }
    }

    TextWrapScope::~TextWrapScope()
    {
        if (m_active)
        {
            ImGui::PopTextWrapPos();
        }
    }

    void TextWrappedDisabled(const char* text)
    {
        const TextWrapScope wrap;
        ImGui::TextDisabled("%s", text);
    }
}
