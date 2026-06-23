#include "app/scene/SceneObjectStore.h"

const std::vector<SceneObject>& SceneObjectStore::Objects() const
{
    return m_objects;
}

std::vector<SceneObject>& SceneObjectStore::Objects()
{
    return m_objects;
}

SceneObject& SceneObjectStore::At(std::size_t index)
{
    return m_objects.at(index);
}

const SceneObject& SceneObjectStore::At(std::size_t index) const
{
    return m_objects.at(index);
}

SceneObjectId SceneObjectStore::AllocateId()
{
    return m_nextObjectId++;
}

void SceneObjectStore::RegisterId(SceneObjectId id)
{
    if (id >= m_nextObjectId)
    {
        m_nextObjectId = id + 1;
    }
}

void SceneObjectStore::FinalizeNewObject(SceneObject& object)
{
    if (object.GetId() == kInvalidSceneObjectId)
    {
        object.SetId(AllocateId());
    }
    else
    {
        RegisterId(object.GetId());
    }
}

int SceneObjectStore::FindIndex(SceneObjectId id) const
{
    if (id == kInvalidSceneObjectId)
    {
        return -1;
    }

    for (std::size_t index = 0; index < m_objects.size(); ++index)
    {
        if (m_objects[index].GetId() == id)
        {
            return static_cast<int>(index);
        }
    }

    return -1;
}

void SceneObjectStore::Clear()
{
    m_objects.clear();
}

SceneObjectId SceneObjectStore::GetNextId() const
{
    return m_nextObjectId;
}

void SceneObjectStore::SetNextId(SceneObjectId nextId)
{
    m_nextObjectId = nextId;
}
