#pragma once

#include "engine/scene/SceneObject.h"
#include "engine/scene/SceneObjectId.h"

#include <cstddef>
#include <vector>

class SceneObjectStore
{
public:
    const std::vector<SceneObject>& Objects() const;
    std::vector<SceneObject>& Objects();

    SceneObject& At(std::size_t index);
    const SceneObject& At(std::size_t index) const;

    SceneObjectId AllocateId();
    void RegisterId(SceneObjectId id);
    void FinalizeNewObject(SceneObject& object);
    int FindIndex(SceneObjectId id) const;

    void Clear();
    SceneObjectId GetNextId() const;
    void SetNextId(SceneObjectId nextId);

private:
    std::vector<SceneObject> m_objects;
    SceneObjectId m_nextObjectId = 1;
};
