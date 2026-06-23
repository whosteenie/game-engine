#pragma once

#include "app/EditorViewportRect.h"

class Scene;

class SceneToolbarPanel
{
public:
    void Draw(Scene& scene, bool sceneViewVisible, const EditorViewportRect& sceneViewRect) const;

    bool& ShowPanel() const { return m_showPanel; }

private:
    mutable bool m_showPanel = true;
};
