#pragma once

#include "app/undo/UndoCommand.h"

class Camera;
class Scene;
class UndoStack;
class EditorSettings;

class LightingPanel
{
public:
    void Draw(
        Scene& scene,
        Camera& camera,
        int viewportWidth,
        int viewportHeight,
        UndoStack* undoStack = nullptr,
        EditorSettings* editorSettings = nullptr) const;

    bool& ShowPanel() const { return m_showPanel; }

private:
    mutable bool m_showPanel = true;
    mutable RendererEditContext m_rendererEditContext;
};
