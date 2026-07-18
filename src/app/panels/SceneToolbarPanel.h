#pragma once

#include "app/editor/EditorViewportRect.h"
#include "app/scene/document/Scene.h"

class UndoStack;

class SceneToolbarPanel
{
public:
    void Draw(
        Scene& scene,
        bool sceneViewVisible,
        const EditorViewportRect& sceneViewRect,
        UndoStack* undoStack = nullptr) const;

    bool& ShowPanel() const { return m_showPanel; }
    SceneViewShadingMode GetShadingMode() const { return m_shadingMode; }

private:
    mutable bool m_showPanel = true;
    mutable SceneViewShadingMode m_shadingMode = SceneViewShadingMode::FullRuntime;
};
