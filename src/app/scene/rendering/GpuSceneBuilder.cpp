#include "app/scene/rendering/GpuSceneBuilder.h"

#include "app/scene/document/Scene.h"
#include "engine/rendering/resources/Material.h"
#include "engine/rendering/resources/Mesh.h"
#include "engine/scene/SceneObject.h"

#include <unordered_set>

namespace
{
    constexpr std::uint32_t kMaterialFlagDoubleSided = 1u << 1;

    std::uint32_t ToTexCoordSet(const int texCoordSet)
    {
        return texCoordSet > 0 ? 1u : 0u;
    }
    void CollectRenderableSelectionIndices(
        const Scene& scene,
        const int objectIndex,
        std::unordered_set<int>& selectedRenderableIndices)
    {
        const std::vector<SceneObject>& objects = scene.GetObjects();
        if (objectIndex < 0 || static_cast<std::size_t>(objectIndex) >= objects.size())
        {
            return;
        }

        if (objects[static_cast<std::size_t>(objectIndex)].IsRenderable())
        {
            selectedRenderableIndices.insert(objectIndex);
        }

        for (const int childIndex : scene.GetChildren(objectIndex))
        {
            CollectRenderableSelectionIndices(scene, childIndex, selectedRenderableIndices);
        }
    }

    int FindFirstRenderableSelectionIndex(const Scene& scene, const int objectIndex)
    {
        const std::vector<SceneObject>& objects = scene.GetObjects();
        if (objectIndex < 0 || static_cast<std::size_t>(objectIndex) >= objects.size())
        {
            return -1;
        }

        if (objects[static_cast<std::size_t>(objectIndex)].IsRenderable())
        {
            return objectIndex;
        }

        for (const int childIndex : scene.GetChildren(objectIndex))
        {
            const int descendantIndex = FindFirstRenderableSelectionIndex(scene, childIndex);
            if (descendantIndex >= 0)
            {
                return descendantIndex;
            }
        }

        return -1;
    }
}

void GpuSceneBuilder::Build(GpuScene& gpuScene, const Scene& scene, const GpuScene::PreviousWorldMap& previousWorldByObjectId)
{
    gpuScene.Clear();

    const std::vector<SceneObject>& objects = scene.GetObjects();
    gpuScene.m_instances.reserve(objects.size());
    gpuScene.m_objectIndexToInstanceId.assign(objects.size(), 0xFFFFFFFFu);

    std::unordered_set<SceneObjectId> seenEditorObjectIds;
    seenEditorObjectIds.reserve(objects.size());

    for (std::size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex)
    {
        const SceneObject& object = objects[objectIndex];
        if (!object.IsRenderable())
        {
            continue;
        }

        Mesh* mesh = object.GetMesh();
        if (mesh == nullptr || !object.HasMaterial())
        {
            ++gpuScene.m_diagnostics.invalidRenderableCount;
            gpuScene.m_diagnostics.valid = false;
            continue;
        }

        const SceneObjectId editorObjectId = object.GetId();
        if (editorObjectId == kInvalidSceneObjectId)
        {
            ++gpuScene.m_diagnostics.invalidEditorObjectIdCount;
            gpuScene.m_diagnostics.valid = false;
        }
        else if (!seenEditorObjectIds.insert(editorObjectId).second)
        {
            ++gpuScene.m_diagnostics.duplicateEditorObjectIdCount;
            gpuScene.m_diagnostics.valid = false;
        }

        const Material& material = object.GetMaterial();
        GpuSceneInstanceRecord instance{};
        instance.instanceId = static_cast<std::uint32_t>(gpuScene.m_instances.size());
        instance.objectIndex = static_cast<std::uint32_t>(objectIndex);
        instance.editorObjectId = editorObjectId;
        instance.meshId = gpuScene.GetOrCreateMeshAssetId(*mesh);
        instance.materialId = gpuScene.GetOrCreateMaterialId(material);
        instance.world = scene.GetWorldMatrix(static_cast<int>(objectIndex));
        const auto previousWorld = previousWorldByObjectId.find(editorObjectId);
        if (previousWorld != previousWorldByObjectId.end())
        {
            instance.prevWorld = previousWorld->second;
            ++gpuScene.m_diagnostics.previousWorldResolvedCount;
        }
        else
        {
            instance.prevWorld = instance.world;
            ++gpuScene.m_diagnostics.previousWorldInitializedCount;
        }
        if (object.CastsShadow())
        {
            instance.flags |= GpuSceneInstanceFlags::CastsShadow;
        }
        if (object.ReceivesShadow())
        {
            instance.flags |= GpuSceneInstanceFlags::ReceivesShadow;
        }
        if (material.IsDoubleSided())
        {
            instance.flags |= GpuSceneInstanceFlags::DoubleSided;
        }

        gpuScene.m_objectIndexToInstanceId[objectIndex] = instance.instanceId;
        gpuScene.m_editorObjectIdToInstanceIds[editorObjectId].push_back(instance.instanceId);
        gpuScene.m_instances.push_back(instance);
    }
}


std::uint32_t CountSelectedRenderInstances(const GpuScene& gpuScene, const Scene& scene)
{
    std::unordered_set<int> selectedObjectIndices;
    for (const int selectedIndex : scene.GetSelection().indices)
    {
        CollectRenderableSelectionIndices(scene, selectedIndex, selectedObjectIndices);
    }

    std::uint32_t selectedInstanceCount = 0;
    for (const int objectIndex : selectedObjectIndices)
    {
        if (objectIndex >= 0 && gpuScene.FindInstanceForObjectIndex(static_cast<std::uint32_t>(objectIndex)) != 0xFFFFFFFFu)
        {
            ++selectedInstanceCount;
        }
    }

    return selectedInstanceCount;
}

const GpuSceneInstanceRecord* FindPrimarySelectionInstance(const GpuScene& gpuScene, const Scene& scene)
{
    const int primaryRenderableIndex = FindFirstRenderableSelectionIndex(scene, scene.GetPrimarySelection());
    if (primaryRenderableIndex < 0)
    {
        return nullptr;
    }

    const std::uint32_t instanceId = gpuScene.FindInstanceForObjectIndex(static_cast<std::uint32_t>(primaryRenderableIndex));
    return gpuScene.FindInstance(instanceId);
}

