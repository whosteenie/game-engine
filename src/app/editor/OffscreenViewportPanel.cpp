#include "app/editor/OffscreenViewportPanel.h"

#include <imgui_internal.h>

#include <algorithm>

namespace
{
    constexpr ImU32 kPlaceholderPrimaryColor = IM_COL32(130, 130, 140, 255);
    constexpr ImU32 kPlaceholderSecondaryColor = IM_COL32(100, 100, 110, 255);
}

namespace OffscreenViewportPanel
{
    void ResetFrameState(State& state)
    {
        state.interactionRect = {};
        state.compositeDrawList = nullptr;
        state.hasCompositeTarget = false;
    }

    void OnPanelHidden(State& state)
    {
        state.renderWidth = 0;
        state.renderHeight = 0;
        state.resizeStabilizer.Reset();
        state.hasReadyCompositeFrame = false;
    }

    bool HasValidRenderTarget(const State& state)
    {
        return state.showPanel && state.renderWidth > 0 && state.renderHeight > 0;
    }

    bool IsLiveResizePending(const State& state)
    {
        return state.resizeStabilizer.IsPending();
    }

    std::uintptr_t GetFramebuffer(const State& state)
    {
        return state.framebuffer.GetFramebuffer();
    }

    std::uintptr_t GetColorTexture(const State& state)
    {
        return state.framebuffer.GetColorTexture();
    }

    void EnsureFramebufferSized(const State& state)
    {
        if (!HasValidRenderTarget(state))
        {
            return;
        }

        (void)state.framebuffer.Resize(state.renderWidth, state.renderHeight);
    }

    void ClearRenderTarget(const State& state)
    {
        if (state.framebuffer.IsValid())
        {
            state.framebuffer.ClearRenderTarget();
        }
    }

    bool CanCompositeFrame(const State& state, const bool willRenderThisFrame)
    {
        if (!state.framebuffer.IsValid() || state.framebuffer.GetColorTexture() == 0)
        {
            return false;
        }

        return state.hasReadyCompositeFrame || willRenderThisFrame;
    }

    void CompositeRenderedFrame(State& state)
    {
        if (!state.hasCompositeTarget
            || state.compositeDrawList == nullptr
            || !state.framebuffer.IsValid()
            || state.framebuffer.GetColorTexture() == 0)
        {
            return;
        }

        state.framebuffer.EnsureShaderResourceState();

        const ImTextureID textureId = static_cast<ImTextureID>(state.framebuffer.GetColorTexture());
        state.compositeDrawList->AddImage(textureId, state.compositeMin, state.compositeMax);
        state.hasReadyCompositeFrame = true;
        state.hasCompositeTarget = false;
        state.compositeDrawList = nullptr;
    }

    bool UpdateRenderSize(State& state, const ImVec2& available)
    {
        const ImVec2 framebufferScale = ImGui::GetIO().DisplayFramebufferScale;
        const int requestedWidth =
            std::max(1, static_cast<int>(available.x * framebufferScale.x + 0.5f));
        const int requestedHeight =
            std::max(1, static_cast<int>(available.y * framebufferScale.y + 0.5f));

        // Keep the committed framebuffer while the panel is moving. DrawViewportRegion already
        // scales that texture to the current ImGui rectangle, so no GPU target or Streamline
        // feature needs to be recreated until the requested extent has settled.
        const ViewportResizeStabilizer::Decision decision = state.resizeStabilizer.Update(
            state.renderWidth,
            state.renderHeight,
            requestedWidth,
            requestedHeight,
            ImGui::GetTime());
        if (decision == ViewportResizeStabilizer::Decision::Commit)
        {
            state.renderWidth = requestedWidth;
            state.renderHeight = requestedHeight;
            state.hasReadyCompositeFrame = false;
            return true;
        }

        return false;
    }

    ViewportRegion DrawViewportRegion(State& state, const ImVec2& available, const bool canComposite)
    {
        ImGui::Dummy(available);
        const ImVec2 cursor = ImGui::GetItemRectMin();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 regionMax(cursor.x + available.x, cursor.y + available.y);
        drawList->AddRectFilled(cursor, regionMax, kBackgroundColor);

        if (canComposite)
        {
            state.compositeDrawList = drawList;
            state.compositeMin = cursor;
            state.compositeMax = regionMax;
            state.hasCompositeTarget = true;

            // Show the last composited frame immediately so tab switches do not flash gray
            // while the GPU render for this frame is still in flight.
            if (state.hasReadyCompositeFrame)
            {
                state.framebuffer.EnsureShaderResourceState();
                const ImTextureID textureId =
                    static_cast<ImTextureID>(state.framebuffer.GetColorTexture());
                drawList->AddImage(textureId, cursor, regionMax);
            }
        }

        ViewportRegion region{};
        region.imageMin = ImGui::GetItemRectMin();
        region.imageMax = ImGui::GetItemRectMax();
        region.imageSize = ImVec2(region.imageMax.x - region.imageMin.x, region.imageMax.y - region.imageMin.y);
        return region;
    }

    void DrawCenteredPlaceholder(
        const ImVec2& cursor,
        const ImVec2& available,
        const char* primaryLabel,
        const char* secondaryLabel)
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
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
            kPlaceholderPrimaryColor,
            primaryLabel);

        if (secondaryLabel != nullptr)
        {
            textY += primarySize.y + lineSpacing;
            drawList->AddText(
                ImVec2(cursor.x + (available.x - secondarySize.x) * 0.5f, textY),
                kPlaceholderSecondaryColor,
                secondaryLabel);
        }
    }

    void UpdateInteractionRect(
        State& state,
        const ImVec2& imageMin,
        const ImVec2& imageSize,
        const bool requireRootWindowHover)
    {
        const ImVec2 framebufferScale = ImGui::GetIO().DisplayFramebufferScale;
        state.interactionRect.valid = state.renderWidth > 0 && state.renderHeight > 0;
        state.interactionRect.screenX = imageMin.x;
        state.interactionRect.screenY = imageMin.y;
        state.interactionRect.screenWidth = imageSize.x;
        state.interactionRect.screenHeight = imageSize.y;
        state.interactionRect.framebufferX = static_cast<int>(imageMin.x * framebufferScale.x);
        state.interactionRect.framebufferY = static_cast<int>(imageMin.y * framebufferScale.y);
        state.interactionRect.width = state.renderWidth;
        state.interactionRect.height = state.renderHeight;

        state.interactionRect.hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);
        if (requireRootWindowHover)
        {
            state.interactionRect.hovered =
                state.interactionRect.hovered
                && ImGui::IsWindowHovered(
                    ImGuiHoveredFlags_AllowWhenBlockedByPopup | ImGuiHoveredFlags_RootWindow);
            state.interactionRect.imguiWindow = ImGui::GetCurrentWindow();
        }
        else
        {
            state.interactionRect.imguiWindow = nullptr;
        }
    }
}
