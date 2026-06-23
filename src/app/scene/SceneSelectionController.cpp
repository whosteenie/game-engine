#include "app/scene/SceneSelectionController.h"

#include "app/scene/SceneObjectStore.h"
#include "engine/scene/SceneObject.h"

#include <algorithm>

const SceneSelection& SceneSelectionController::Get() const
{
    return m_selection;
}

void SceneSelectionController::SetState(const SceneSelection& selection)
{
    m_selection = selection;
}

int SceneSelectionController::Primary() const
{
    return m_selection.primary;
}

bool SceneSelectionController::HasSelection() const
{
    return !m_selection.indices.empty();
}

bool SceneSelectionController::IsSelected(int objectIndex) const
{
    if (objectIndex < 0)
    {
        return false;
    }

    return std::find(m_selection.indices.begin(), m_selection.indices.end(), objectIndex)
        != m_selection.indices.end();
}

void SceneSelectionController::Set(
    const std::vector<SceneObject>& objects,
    const std::vector<int>& indices,
    int primary)
{
    if (objects.empty())
    {
        Clear();
        return;
    }

    m_selection.indices.clear();
    for (int index : indices)
    {
        if (index < 0 || static_cast<std::size_t>(index) >= objects.size())
        {
            continue;
        }

        if (std::find(m_selection.indices.begin(), m_selection.indices.end(), index)
            != m_selection.indices.end())
        {
            continue;
        }

        m_selection.indices.push_back(index);
    }

    if (primary >= 0
        && static_cast<std::size_t>(primary) < objects.size()
        && std::find(m_selection.indices.begin(), m_selection.indices.end(), primary)
            != m_selection.indices.end())
    {
        m_selection.primary = primary;
    }
    else
    {
        m_selection.primary = m_selection.indices.empty() ? -1 : m_selection.indices.back();
    }
}

void SceneSelectionController::SelectSingle(const std::vector<SceneObject>& objects, int objectIndex)
{
    if (objects.empty())
    {
        Clear();
        return;
    }

    if (objectIndex < 0)
    {
        Clear();
        return;
    }

    if (static_cast<std::size_t>(objectIndex) >= objects.size())
    {
        objectIndex = static_cast<int>(objects.size()) - 1;
    }

    m_selection.indices = {objectIndex};
    m_selection.primary = objectIndex;
}

void SceneSelectionController::Toggle(const std::vector<SceneObject>& objects, int objectIndex)
{
    if (objects.empty() || objectIndex < 0 || static_cast<std::size_t>(objectIndex) >= objects.size())
    {
        return;
    }

    const auto iterator = std::find(m_selection.indices.begin(), m_selection.indices.end(), objectIndex);
    if (iterator != m_selection.indices.end())
    {
        m_selection.indices.erase(iterator);
        if (m_selection.primary == objectIndex)
        {
            m_selection.primary = m_selection.indices.empty() ? -1 : m_selection.indices.back();
        }
        return;
    }

    m_selection.indices.push_back(objectIndex);
    m_selection.primary = objectIndex;
}

void SceneSelectionController::Add(const std::vector<SceneObject>& objects, const std::vector<int>& indices)
{
    if (objects.empty())
    {
        Clear();
        return;
    }

    for (int index : indices)
    {
        if (index < 0 || static_cast<std::size_t>(index) >= objects.size())
        {
            continue;
        }

        if (IsSelected(index))
        {
            continue;
        }

        m_selection.indices.push_back(index);
        if (m_selection.primary < 0)
        {
            m_selection.primary = index;
        }
    }
}

void SceneSelectionController::Clear()
{
    m_selection = {};
}

std::vector<SceneObjectId> SceneSelectionController::GetIds(const std::vector<SceneObject>& objects) const
{
    std::vector<SceneObjectId> ids;
    ids.reserve(m_selection.indices.size());
    for (int index : m_selection.indices)
    {
        if (index >= 0 && index < static_cast<int>(objects.size()))
        {
            ids.push_back(objects[static_cast<std::size_t>(index)].GetId());
        }
    }

    return ids;
}

void SceneSelectionController::SetByIds(
    const SceneObjectStore& store,
    const std::vector<SceneObjectId>& ids,
    SceneObjectId primary)
{
    std::vector<int> indices;
    indices.reserve(ids.size());
    for (SceneObjectId id : ids)
    {
        const int index = store.FindIndex(id);
        if (index < 0)
        {
            continue;
        }

        if (std::find(indices.begin(), indices.end(), index) == indices.end())
        {
            indices.push_back(index);
        }
    }

    int primaryIndex = store.FindIndex(primary);
    if (primaryIndex < 0
        || std::find(indices.begin(), indices.end(), primaryIndex) == indices.end())
    {
        primaryIndex = indices.empty() ? -1 : indices.back();
    }

    m_selection.indices = std::move(indices);
    m_selection.primary = primaryIndex;
}

void SceneSelectionController::RemapAfterRemoval(int removedIndex)
{
    std::vector<int> remappedIndices;
    remappedIndices.reserve(m_selection.indices.size());

    for (int index : m_selection.indices)
    {
        if (index == removedIndex)
        {
            continue;
        }

        remappedIndices.push_back(index > removedIndex ? index - 1 : index);
    }

    m_selection.indices = std::move(remappedIndices);

    if (m_selection.primary == removedIndex)
    {
        m_selection.primary = m_selection.indices.empty() ? -1 : m_selection.indices.back();
    }
    else if (m_selection.primary > removedIndex)
    {
        --m_selection.primary;
    }
}

void SceneSelectionController::Sanitize(const std::vector<SceneObject>& objects)
{
    if (objects.empty())
    {
        Clear();
        return;
    }

    const int objectCount = static_cast<int>(objects.size());
    std::vector<int> validIndices;
    validIndices.reserve(m_selection.indices.size());

    for (int index : m_selection.indices)
    {
        if (index < 0 || index >= objectCount)
        {
            continue;
        }

        if (std::find(validIndices.begin(), validIndices.end(), index) == validIndices.end())
        {
            validIndices.push_back(index);
        }
    }

    m_selection.indices = std::move(validIndices);

    if (m_selection.primary < 0
        || m_selection.primary >= objectCount
        || !IsSelected(m_selection.primary))
    {
        m_selection.primary = m_selection.indices.empty() ? -1 : m_selection.indices.back();
    }
}
