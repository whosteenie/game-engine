#include "app/inspector/InspectorTransform.h"

#include "app/editor/EditorWidgets.h"
#include "app/inspector/InspectorMultiEdit.h"
#include "app/scene/document/Scene.h"
#include "engine/scene/SceneObject.h"
#include "engine/scene/Transform.h"

namespace
{
    glm::vec3 MergeEditedComponent(const glm::vec3& current, const MultiVec3& field)
    {
        glm::vec3 merged = current;
        for (int axis = 0; axis < 3; ++axis)
        {
            if (!field.mixed[axis])
            {
                merged[axis] = field.value[axis];
            }
        }

        return merged;
    }
}

WorldTransformState GetObjectWorldTransformState(const Scene& scene, int objectIndex)
{
    Transform worldTransform;
    worldTransform.SetFromMatrix(scene.GetWorldMatrix(objectIndex));

    WorldTransformState state;
    state.position = worldTransform.position;
    state.rotationDegrees = worldTransform.GetRotationDegrees();
    state.scale = worldTransform.scale;
    EditorWidgets::SanitizeSignedZero(state.position);
    EditorWidgets::SanitizeSignedZero(state.rotationDegrees);
    EditorWidgets::SanitizeSignedZero(state.scale);
    return state;
}

void SetObjectWorldPosition(Scene& scene, int objectIndex, const glm::vec3& worldPosition)
{
    const glm::mat4 worldMatrix = scene.GetWorldMatrix(objectIndex);
    const glm::vec3 currentPosition = glm::vec3(worldMatrix[3]);
    const glm::mat4 translationDelta =
        glm::translate(glm::mat4(1.0f), worldPosition - currentPosition);
    scene.SetObjectWorldMatrix(objectIndex, translationDelta * worldMatrix);
}

void SetObjectWorldRotationDegrees(Scene& scene, int objectIndex, const glm::vec3& rotationDegrees)
{
    WorldTransformState state = GetObjectWorldTransformState(scene, objectIndex);

    Transform worldTransform;
    worldTransform.position = state.position;
    worldTransform.SetRotationDegrees(rotationDegrees);
    worldTransform.scale = state.scale;
    scene.SetObjectWorldMatrix(objectIndex, worldTransform.ToMatrix());
}

void SetObjectWorldScale(Scene& scene, int objectIndex, const glm::vec3& worldScale)
{
    WorldTransformState state = GetObjectWorldTransformState(scene, objectIndex);

    Transform worldTransform;
    worldTransform.position = state.position;
    worldTransform.rotation = glm::quat(glm::radians(state.rotationDegrees));
    worldTransform.scale = worldScale;
    scene.SetObjectWorldMatrix(objectIndex, worldTransform.ToMatrix());
}

void ApplyWorldPositionFieldToObjects(
    Scene& scene,
    const std::vector<int>& objectIndices,
    const MultiVec3& field)
{
    for (int objectIndex : objectIndices)
    {
        const WorldTransformState state = GetObjectWorldTransformState(scene, objectIndex);
        SetObjectWorldPosition(scene, objectIndex, MergeEditedComponent(state.position, field));
    }
}

void ApplyWorldRotationFieldToObjects(
    Scene& scene,
    const std::vector<int>& objectIndices,
    const MultiVec3& field)
{
    for (int objectIndex : objectIndices)
    {
        const WorldTransformState state = GetObjectWorldTransformState(scene, objectIndex);
        SetObjectWorldRotationDegrees(
            scene,
            objectIndex,
            MergeEditedComponent(state.rotationDegrees, field));
    }
}

void ApplyWorldScaleFieldToObjects(
    Scene& scene,
    const std::vector<int>& objectIndices,
    const MultiVec3& field)
{
    for (int objectIndex : objectIndices)
    {
        const WorldTransformState state = GetObjectWorldTransformState(scene, objectIndex);
        SetObjectWorldScale(scene, objectIndex, MergeEditedComponent(state.scale, field));
    }
}

void ResetTransformsOnObjects(Scene& scene, const std::vector<int>& objectIndices)
{
    for (int objectIndex : objectIndices)
    {
        scene.GetSceneObject(static_cast<std::size_t>(objectIndex)).GetTransform().Reset();
    }

    if (!objectIndices.empty())
    {
        scene.MarkDirty();
    }
}
