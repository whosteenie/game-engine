#pragma once

#include "app/UndoCommand.h"

#include <vector>

class Scene;
class UndoStack;

class SceneInspectorPanel
{
public:
    void Draw(Scene& scene, UndoStack* undoStack = nullptr) const;

    bool& ShowPanel() const { return m_showPanel; }

private:
    mutable bool m_showPanel = true;
    mutable TransformEditContext m_transformEditContext;
    mutable std::vector<int> m_transformEditSelection;
    mutable MaterialEditContext m_materialEditContext;
    mutable std::vector<int> m_materialEditSelection;
    mutable LightEditContext m_lightEditContext;
    mutable std::vector<int> m_lightEditSelection;
};
