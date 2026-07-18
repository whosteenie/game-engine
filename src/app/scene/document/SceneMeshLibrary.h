#pragma once

#include "app/project/SceneImportedMeshPool.h"
#include "engine/scene/ScenePrimitive.h"

#include <memory>
#include <unordered_set>
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
    bool IsImportedMesh(const Mesh* mesh) const;
    Mesh* AdoptImportedMesh(std::unique_ptr<Mesh> mesh);
    Mesh* AdoptClonedImportedMesh(const Mesh& source);
    std::unique_ptr<Mesh> ExtractImportedMesh(Mesh* mesh);
    void PinImportedMesh(Mesh* mesh);

    void PruneUnusedImportedMeshes(const std::vector<SceneObject>& objects);
    void HarvestImportedMeshes(
        const std::vector<SceneObject>& objects,
        ImportedMeshReusePool& outPool);
    void ClearImportedMeshes();
    void InvalidatePrimitives();

private:
    void EnsurePrimitives() const;

    float m_floorHalfExtent = 0.0f;
    mutable bool m_primitivesReady = false;
    std::unique_ptr<Mesh> m_cubeMesh;
    std::unique_ptr<Mesh> m_sphereMesh;
    std::unique_ptr<Mesh> m_cylinderMesh;
    std::unique_ptr<Mesh> m_capsuleMesh;
    std::unique_ptr<Mesh> m_planeMesh;
    std::vector<std::unique_ptr<Mesh>> m_importedMeshes;
    std::unordered_set<Mesh*> m_pinnedImportedMeshes;
};
