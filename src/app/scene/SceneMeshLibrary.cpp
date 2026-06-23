#include "app/scene/SceneMeshLibrary.h"

#include "engine/rendering/Mesh.h"
#include "engine/scene/SceneObject.h"
#include "primitives/Capsule.h"
#include "primitives/Cube.h"
#include "primitives/Cylinder.h"
#include "primitives/Plane.h"
#include "primitives/Sphere.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

SceneMeshLibrary::SceneMeshLibrary(float floorHalfExtent)
    : m_cubeMesh(CreateCubeMesh()),
      m_sphereMesh(CreateSphereMesh()),
      m_cylinderMesh(CreateCylinderMesh()),
      m_capsuleMesh(CreateCapsuleMesh()),
      m_planeMesh(CreatePlaneMesh(floorHalfExtent))
{
}

SceneMeshLibrary::~SceneMeshLibrary() = default;

Mesh* SceneMeshLibrary::GetPrimitive(ScenePrimitive primitive) const
{
    switch (primitive)
    {
    case ScenePrimitive::Cube:
        return m_cubeMesh.get();
    case ScenePrimitive::Sphere:
        return m_sphereMesh.get();
    case ScenePrimitive::Cylinder:
        return m_cylinderMesh.get();
    case ScenePrimitive::Capsule:
        return m_capsuleMesh.get();
    case ScenePrimitive::Plane:
        return m_planeMesh.get();
    }

    return m_cubeMesh.get();
}

Mesh* SceneMeshLibrary::AdoptImportedMesh(std::unique_ptr<Mesh> mesh)
{
    m_importedMeshes.push_back(std::move(mesh));
    return m_importedMeshes.back().get();
}

std::unique_ptr<Mesh> SceneMeshLibrary::ExtractImportedMesh(Mesh* mesh)
{
    if (mesh == nullptr)
    {
        return nullptr;
    }

    for (std::unique_ptr<Mesh>& ownedMesh : m_importedMeshes)
    {
        if (ownedMesh.get() != mesh)
        {
            continue;
        }

        std::unique_ptr<Mesh> extracted = std::move(ownedMesh);
        m_importedMeshes.erase(
            std::remove_if(
                m_importedMeshes.begin(),
                m_importedMeshes.end(),
                [](const std::unique_ptr<Mesh>& candidate) { return candidate == nullptr; }),
            m_importedMeshes.end());
        return extracted;
    }

    return nullptr;
}

void SceneMeshLibrary::PruneUnusedImportedMeshes(const std::vector<SceneObject>& objects)
{
    std::unordered_set<Mesh*> referencedMeshes;
    referencedMeshes.reserve(objects.size());

    for (const SceneObject& object : objects)
    {
        if (object.HasMesh())
        {
            referencedMeshes.insert(object.GetMesh());
        }
    }

    m_importedMeshes.erase(
        std::remove_if(
            m_importedMeshes.begin(),
            m_importedMeshes.end(),
            [&](const std::unique_ptr<Mesh>& mesh) {
                return mesh == nullptr || referencedMeshes.find(mesh.get()) == referencedMeshes.end();
            }),
        m_importedMeshes.end());
}

void SceneMeshLibrary::HarvestImportedMeshes(
    const std::vector<SceneObject>& objects,
    ImportedMeshReusePool& outPool)
{
    std::unordered_map<Mesh*, ImportMeshKey> meshKeys;
    meshKeys.reserve(m_importedMeshes.size());

    for (const SceneObject& object : objects)
    {
        if (!object.HasMesh() || object.GetImportAssetPath().empty() || object.GetImportNodeIndex() < 0)
        {
            continue;
        }

        meshKeys[object.GetMesh()] = ImportMeshKey{object.GetImportAssetPath(), object.GetImportNodeIndex()};
    }

    for (std::unique_ptr<Mesh>& mesh : m_importedMeshes)
    {
        if (mesh == nullptr)
        {
            continue;
        }

        const auto iterator = meshKeys.find(mesh.get());
        if (iterator == meshKeys.end())
        {
            continue;
        }

        outPool.try_emplace(iterator->second, std::move(mesh));
    }

    m_importedMeshes.erase(
        std::remove_if(
            m_importedMeshes.begin(),
            m_importedMeshes.end(),
            [](const std::unique_ptr<Mesh>& mesh) { return mesh == nullptr; }),
        m_importedMeshes.end());
}

void SceneMeshLibrary::ClearImportedMeshes()
{
    m_importedMeshes.clear();
}
