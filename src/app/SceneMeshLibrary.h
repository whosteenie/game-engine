#pragma once

#include "app/SceneImportedMeshPool.h"
#include "engine/ScenePrimitive.h"

#include <memory>
#include <vector>

class Mesh;
class SceneObject;

class SceneMeshLibrary
{
public:
    explicit SceneMeshLibrary(float floorHalfExtent);
    ~SceneMeshLibrary();

    SceneMeshLibrary(const SceneMeshLibrary&) = delete;
    SceneMeshLibrary& operator=(const SceneMeshLibrary&) = delete;

    Mesh* GetPrimitive(ScenePrimitive primitive) const;
    Mesh* AdoptImportedMesh(std::unique_ptr<Mesh> mesh);
    std::unique_ptr<Mesh> ExtractImportedMesh(Mesh* mesh);

    void PruneUnusedImportedMeshes(const std::vector<SceneObject>& objects);
    void HarvestImportedMeshes(
        const std::vector<SceneObject>& objects,
        ImportedMeshReusePool& outPool);
    void ClearImportedMeshes();

private:
    std::unique_ptr<Mesh> m_cubeMesh;
    std::unique_ptr<Mesh> m_sphereMesh;
    std::unique_ptr<Mesh> m_cylinderMesh;
    std::unique_ptr<Mesh> m_capsuleMesh;
    std::unique_ptr<Mesh> m_planeMesh;
    std::vector<std::unique_ptr<Mesh>> m_importedMeshes;
};
