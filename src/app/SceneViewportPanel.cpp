#include "app/SceneViewportPanel.h"

#include "app/EditorPanelConstraints.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>

namespace
{
    void UpdateInteractionRect(
        EditorViewportRect& interactionRect,
        const ImVec2& imageMin,
        const ImVec2& imageSize,
        int renderWidth,
        int renderHeight)
    {
        const ImVec2 framebufferScale = ImGui::GetIO().DisplayFramebufferScale;
        interactionRect.valid = renderWidth > 0 && renderHeight > 0;
        interactionRect.screenX = imageMin.x;
        interactionRect.screenY = imageMin.y;
        interactionRect.screenWidth = imageSize.x;
        interactionRect.screenHeight = imageSize.y;
        interactionRect.framebufferX = static_cast<int>(imageMin.x * framebufferScale.x);
        interactionRect.framebufferY = static_cast<int>(imageMin.y * framebufferScale.y);
        interactionRect.width = renderWidth;
        interactionRect.height = renderHeight;
    }
}

bool SceneViewportPanel::HasValidRenderTarget() const
{
    return m_showPanel && m_renderWidth > 0 && m_renderHeight > 0;
}

unsigned int SceneViewportPanel::GetFramebuffer() const
{
    return m_framebuffer.GetFramebuffer();
}

unsigned int SceneViewportPanel::GetColorTexture() const
{
    return m_framebuffer.GetColorTexture();
}

void SceneViewportPanel::EnsureFramebufferSized() const
{
    if (!HasValidRenderTarget())
    {
        return;
    }

    m_framebuffer.Resize(m_renderWidth, m_renderHeight);
}

void SceneViewportPanel::Draw()
{
    m_interactionRect = {};

    EditorPanelConstraints::ApplySceneViewPanel();
    if (!EditorPanelConstraints::BeginDockedPanel("Scene View", m_showPanel))
    {
        m_renderWidth = 0;
        m_renderHeight = 0;
        return;
    }

    const ImVec2 available = ImGui::GetContentRegionAvail();
    const ImVec2 framebufferScale = ImGui::GetIO().DisplayFramebufferScale;
    m_renderWidth = std::max(1, static_cast<int>(available.x * framebufferScale.x));
    m_renderHeight = std::max(1, static_cast<int>(available.y * framebufferScale.y));

    if (m_framebuffer.IsValid() && m_framebuffer.GetColorTexture() != 0)
    {
        const ImTextureID textureId =
            static_cast<ImTextureID>(static_cast<intptr_t>(m_framebuffer.GetColorTexture()));
        ImGui::Image(textureId, available, ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
    }
    else
    {
        ImGui::Dummy(available);
        const ImVec2 cursor = ImGui::GetItemRectMin();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 regionMax(cursor.x + available.x, cursor.y + available.y);
        drawList->AddRectFilled(cursor, regionMax, IM_COL32(34, 34, 38, 255));

        constexpr const char* kPlaceholderLabel = "Scene View";
        const ImVec2 textSize = ImGui::CalcTextSize(kPlaceholderLabel);
        drawList->AddText(
            ImVec2(cursor.x + (available.x - textSize.x) * 0.5f, cursor.y + (available.y - textSize.y) * 0.5f),
            IM_COL32(130, 130, 140, 255),
            kPlaceholderLabel);
    }

    const ImVec2 imageMin = ImGui::GetItemRectMin();
    const ImVec2 imageMax = ImGui::GetItemRectMax();
    const ImVec2 imageSize(imageMax.x - imageMin.x, imageMax.y - imageMin.y);
    m_interactionRect.hovered =
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup)
        && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup | ImGuiHoveredFlags_RootWindow);
    m_interactionRect.imguiWindow = ImGui::GetCurrentWindow();
    UpdateInteractionRect(m_interactionRect, imageMin, imageSize, m_renderWidth, m_renderHeight);

    ImGui::End();
}
