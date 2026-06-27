#include "app/panels/GameViewportPanel.h"

#include "app/editor/EditorPanelConstraints.h"

#include <imgui.h>

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
        interactionRect.hovered = false;
    }
}

bool GameViewportPanel::HasValidRenderTarget() const
{
    return m_showPanel && m_renderWidth > 0 && m_renderHeight > 0;
}

std::uintptr_t GameViewportPanel::GetFramebuffer() const
{
    return m_framebuffer.GetFramebuffer();
}

std::uintptr_t GameViewportPanel::GetColorTexture() const
{
    return m_framebuffer.GetColorTexture();
}

void GameViewportPanel::EnsureFramebufferSized() const
{
    if (!HasValidRenderTarget())
    {
        return;
    }

    (void)m_framebuffer.Resize(m_renderWidth, m_renderHeight);
}

void GameViewportPanel::ClearRenderTarget() const
{
    if (m_framebuffer.IsValid())
    {
        m_framebuffer.ClearRenderTarget();
    }
}

void GameViewportPanel::DrawPlaceholder(const ImVec2& available, const bool hasSceneCamera)
{
    ImGui::Dummy(available);
    const ImVec2 cursor = ImGui::GetItemRectMin();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 regionMax(cursor.x + available.x, cursor.y + available.y);
    drawList->AddRectFilled(cursor, regionMax, IM_COL32(34, 34, 38, 255));

    const char* primaryLabel = hasSceneCamera ? "Game View" : "No camera in scene";
    const char* secondaryLabel =
        hasSceneCamera ? nullptr : "Add a Camera object to preview";
    const ImVec2 primarySize = ImGui::CalcTextSize(primaryLabel);
    const float lineSpacing = ImGui::GetTextLineHeightWithSpacing() - ImGui::GetTextLineHeight();
    float totalHeight = primarySize.y;
    ImVec2 secondarySize(0.0f, 0.0f);
    if (secondaryLabel != nullptr)
    {
        secondarySize = ImGui::CalcTextSize(secondaryLabel);
        totalHeight += lineSpacing + secondarySize.y;
    }

    float textY = cursor.y + (available.y - totalHeight) * 0.5f;
    drawList->AddText(
        ImVec2(cursor.x + (available.x - primarySize.x) * 0.5f, textY),
        IM_COL32(130, 130, 140, 255),
        primaryLabel);

    if (secondaryLabel != nullptr)
    {
        textY += primarySize.y + lineSpacing;
        drawList->AddText(
            ImVec2(cursor.x + (available.x - secondarySize.x) * 0.5f, textY),
            IM_COL32(100, 100, 110, 255),
            secondaryLabel);
    }
}

void GameViewportPanel::Draw(const bool hasSceneCamera)
{
    m_interactionRect = {};
    m_compositeDrawList = nullptr;
    m_hasCompositeTarget = false;

    EditorPanelConstraints::ApplySceneViewPanel();
    if (!EditorPanelConstraints::BeginDockedPanel("Game View", m_showPanel))
    {
        m_renderWidth = 0;
        m_renderHeight = 0;
        return;
    }

    const ImVec2 available = ImGui::GetContentRegionAvail();
    const ImVec2 framebufferScale = ImGui::GetIO().DisplayFramebufferScale;
    const int requestedWidth = std::max(1, static_cast<int>(available.x * framebufferScale.x + 0.5f));
    const int requestedHeight = std::max(1, static_cast<int>(available.y * framebufferScale.y + 0.5f));

    if (m_renderWidth <= 0
        || m_renderHeight <= 0
        || std::abs(requestedWidth - m_renderWidth) > 1
        || std::abs(requestedHeight - m_renderHeight) > 1)
    {
        m_renderWidth = requestedWidth;
        m_renderHeight = requestedHeight;
    }

    const bool canCompositeFrame =
        hasSceneCamera && m_framebuffer.IsValid() && m_framebuffer.GetColorTexture() != 0;

    if (canCompositeFrame)
    {
        ImGui::Dummy(available);
        const ImVec2 cursor = ImGui::GetItemRectMin();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 regionMax(cursor.x + available.x, cursor.y + available.y);
        drawList->AddRectFilled(cursor, regionMax, IM_COL32(34, 34, 38, 255));

        m_compositeDrawList = drawList;
        m_compositeMin = cursor;
        m_compositeMax = regionMax;
        m_hasCompositeTarget = true;
    }
    else
    {
        DrawPlaceholder(available, hasSceneCamera);
    }

    const ImVec2 imageMin = ImGui::GetItemRectMin();
    const ImVec2 imageMax = ImGui::GetItemRectMax();
    const ImVec2 imageSize(imageMax.x - imageMin.x, imageMax.y - imageMin.y);
    UpdateInteractionRect(m_interactionRect, imageMin, imageSize, m_renderWidth, m_renderHeight);
    m_interactionRect.hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);

    ImGui::End();
}

void GameViewportPanel::CompositeRenderedFrame()
{
    if (!m_hasCompositeTarget
        || m_compositeDrawList == nullptr
        || !m_framebuffer.IsValid()
        || m_framebuffer.GetColorTexture() == 0)
    {
        return;
    }

    const ImTextureID textureId = static_cast<ImTextureID>(m_framebuffer.GetColorTexture());
    m_compositeDrawList->AddImage(textureId, m_compositeMin, m_compositeMax);
    m_hasCompositeTarget = false;
    m_compositeDrawList = nullptr;
}
