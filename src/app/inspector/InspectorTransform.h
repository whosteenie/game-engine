#pragma once

#include "app/inspector/InspectorMultiEdit.h"

#include <glm/glm.hpp>
#include <vector>

class Scene;
struct Transform;

struct WorldTransformState
{
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec3 rotationDegrees = glm::vec3(0.0f);
    glm::vec3 scale = glm::vec3(1.0f);
};

WorldTransformState GetObjectWorldTransformState(const Scene& scene, int objectIndex);
void SetObjectWorldPosition(Scene& scene, int objectIndex, const glm::vec3& worldPosition);
void SetObjectWorldRotationDegrees(Scene& scene, int objectIndex, const glm::vec3& rotationDegrees);
void SetObjectWorldScale(Scene& scene, int objectIndex, const glm::vec3& worldScale);

void ApplyWorldPositionFieldToObjects(
    Scene& scene,
    const std::vector<int>& objectIndices,
    const MultiVec3& field);
void ApplyWorldRotationFieldToObjects(
    Scene& scene,
    const std::vector<int>& objectIndices,
    const MultiVec3& field);
void ApplyWorldScaleFieldToObjects(
    Scene& scene,
    const std::vector<int>& objectIndices,
    const MultiVec3& field);
void ResetTransformsOnObjects(Scene& scene, const std::vector<int>& objectIndices);
