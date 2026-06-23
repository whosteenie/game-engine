#pragma once

#include "app/UndoCommand.h"

class Camera;
class Scene;
class UndoStack;

class LightingPanel
{
public:
    void Draw(
        Scene& scene,
        const Camera& camera,
        int viewportWidth,
        int viewportHeight,
        UndoStack* undoStack = nullptr) const;

    bool& ShowPanel() const { return m_showPanel; }

private:
    mutable bool m_showPanel = true;
    mutable RendererEditContext m_rendererEditContext;
};
