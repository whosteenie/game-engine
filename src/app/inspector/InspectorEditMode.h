#pragma once

#include <vector>

class Scene;

// Controls how inspector sections behave with multi-selection.
// Future custom components should declare which mode they use.
enum class InspectorEditMode
{
    // Hidden when more than one object is selected (e.g. Material, Light type).
    SingleOnly,
    // Shown in multi-select only when every selected object shares the component (reserved).
    MultiHomogeneous,
    // Shown for any non-empty selection; fields use multi-edit widgets (e.g. Transform, flags).
    MultiAny,
};

enum class InspectorSectionKind
{
    Name,
    Transform,
    Object,
    Light,
    Material,
};

InspectorEditMode GetInspectorSectionEditMode(InspectorSectionKind section);

bool ShouldShowInspectorSection(
    InspectorSectionKind section,
    const Scene& scene,
    const std::vector<int>& selectedIndices);
