#pragma once

#include "app/SceneSelection.h"
#include "engine/SceneObjectId.h"

#include <vector>

class SceneObject;
class SceneObjectStore;

class SceneSelectionController
{
public:
    const SceneSelection& Get() const;
    void SetState(const SceneSelection& selection);

    int Primary() const;
    bool HasSelection() const;
    bool IsSelected(int objectIndex) const;

    void Set(const std::vector<SceneObject>& objects, const std::vector<int>& indices, int primary);
    void SelectSingle(const std::vector<SceneObject>& objects, int objectIndex);
    void Toggle(const std::vector<SceneObject>& objects, int objectIndex);
    void Add(const std::vector<SceneObject>& objects, const std::vector<int>& indices);
    void Clear();

    std::vector<SceneObjectId> GetIds(const std::vector<SceneObject>& objects) const;
    void SetByIds(const SceneObjectStore& store, const std::vector<SceneObjectId>& ids, SceneObjectId primary);
    void RemapAfterRemoval(int removedIndex);
    void Sanitize(const std::vector<SceneObject>& objects);

private:
    SceneSelection m_selection;
};
