#include "app/InspectorEditMode.h"

#include "app/Scene.h"
#include "engine/SceneObject.h"

#include <algorithm>

InspectorEditMode GetInspectorSectionEditMode(InspectorSectionKind section)
{
    switch (section)
    {
    case InspectorSectionKind::Name:
    case InspectorSectionKind::Light:
    case InspectorSectionKind::Material:
        return InspectorEditMode::SingleOnly;
    case InspectorSectionKind::Transform:
    case InspectorSectionKind::Object:
        return InspectorEditMode::MultiAny;
    }

    return InspectorEditMode::SingleOnly;
}

bool ShouldShowInspectorSection(
    InspectorSectionKind section,
    const Scene& scene,
    const std::vector<int>& selectedIndices)
{
    if (selectedIndices.empty())
    {
        return false;
    }

    switch (GetInspectorSectionEditMode(section))
    {
    case InspectorEditMode::SingleOnly:
        if (selectedIndices.size() != 1)
        {
            return false;
        }
        break;
    case InspectorEditMode::MultiHomogeneous:
    case InspectorEditMode::MultiAny:
        break;
    }

    switch (section)
    {
    case InspectorSectionKind::Name:
        return selectedIndices.size() == 1;
    case InspectorSectionKind::Transform:
        return !selectedIndices.empty();
    case InspectorSectionKind::Object:
        return true;
    case InspectorSectionKind::Light:
        return selectedIndices.size() == 1
            && scene.GetObject(static_cast<std::size_t>(selectedIndices.front())).HasLight();
    case InspectorSectionKind::Material:
        return selectedIndices.size() == 1
            && scene.GetObject(static_cast<std::size_t>(selectedIndices.front())).HasMaterial();
    }

    return false;
}
