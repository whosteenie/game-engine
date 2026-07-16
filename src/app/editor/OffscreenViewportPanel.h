#pragma once

#include "app/editor/EditorViewportRect.h"
#include "app/editor/ViewportResizeStabilizer.h"

#include "engine/rendering/Framebuffer.h"

#include <imgui.h>

namespace OffscreenViewportPanel
{
    inline constexpr ImU32 kBackgroundColor = IM_COL32(34, 34, 38, 255);

    struct State
    {
        bool showPanel = true;
        mutable Framebuffer framebuffer;
        mutable int renderWidth = 0;
        mutable int renderHeight = 0;
        ViewportResizeStabilizer resizeStabilizer{};
        mutable EditorViewportRect interactionRect{};
        ImDrawList* compositeDrawList = nullptr;
        ImVec2 compositeMin{};
        ImVec2 compositeMax{};
        bool hasCompositeTarget = false;
        bool hasReadyCompositeFrame = false;
    };

    struct ViewportRegion
    {
        ImVec2 imageMin{};
        ImVec2 imageMax{};
        ImVec2 imageSize{};
    };

    void ResetFrameState(State& state);
    void OnPanelHidden(State& state);

    bool HasValidRenderTarget(const State& state);
    bool IsLiveResizePending(const State& state);
    bool HasReadyCompositeFrame(const State& state);
    std::uintptr_t GetFramebuffer(const State& state);
    std::uintptr_t GetColorTexture(const State& state);
    void EnsureFramebufferSized(const State& state);
    void ClearRenderTarget(const State& state);
    bool CanCompositeFrame(const State& state, bool willRenderThisFrame);
    void CompositeRenderedFrame(State& state);
    void InvalidateCompositeFrame(State& state);

    bool UpdateRenderSize(State& state, const ImVec2& available);
    ViewportRegion DrawViewportRegion(State& state, const ImVec2& available, bool canComposite);
    void DrawCenteredPlaceholder(
        const ImVec2& cursor,
        const ImVec2& available,
        const char* primaryLabel,
        const char* secondaryLabel = nullptr);

    void UpdateInteractionRect(
        State& state,
        const ImVec2& imageMin,
        const ImVec2& imageSize,
        bool requireRootWindowHover);
}
