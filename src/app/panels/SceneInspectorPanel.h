#pragma once

#include "app/undo/UndoCommand.h"

#include "engine/scene/SceneObjectId.h"

#include <string>
#include <vector>

class Scene;
class UndoStack;

class SceneInspectorPanel
{
public:
    void Draw(Scene& scene, UndoStack* undoStack = nullptr) const;

    bool& ShowPanel() const { return m_showPanel; }

    void ClearDragInsertLatch() const;
    void UpdateDragInsertLatch(
        int objectIndex,
        int beforeIndex,
        float itemMinX,
        float itemMinY,
        float itemMaxX,
        float itemMaxY,
        bool useBottomInsertLineY = false) const;
    bool HasDragInsertLatchFor(int objectIndex, int beforeIndex) const;
    void DrawDragInsertLatchLine() const;
    void ClearDragInsertLatchUnlessObject(int objectIndex) const;

private:
    mutable bool m_showPanel = true;
    mutable TransformEditContext m_transformEditContext;
    mutable std::vector<int> m_transformEditSelection;
    mutable MaterialEditContext m_materialEditContext;
    mutable std::vector<int> m_materialEditSelection;
    mutable LightEditContext m_lightEditContext;
    mutable std::vector<int> m_lightEditSelection;
    mutable CameraEditContext m_cameraEditContext;
    mutable std::vector<int> m_cameraEditSelection;
    mutable RigidBodyEditContext m_rigidBodyEditContext;
    mutable std::vector<int> m_rigidBodyEditSelection;
    mutable ColliderEditContext m_colliderEditContext;
    mutable std::vector<int> m_colliderEditSelection;
    mutable SceneObjectId m_nameEditObjectId = kInvalidSceneObjectId;
    mutable std::string m_nameEditOldName;
    mutable int m_dragInsertLatchObjectIndex = -1;
    mutable int m_dragInsertLatchBeforeIndex = -1;
    mutable float m_dragInsertLatchLineY = 0.0f;
    mutable float m_dragInsertLatchLineMinX = 0.0f;
    mutable float m_dragInsertLatchLineMaxX = 0.0f;
};
