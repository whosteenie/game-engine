#pragma once

#include "engine/Light.h"
#include "engine/ScenePrimitive.h"

class Scene;

struct SceneSpawnCounters
{
    int directionalLight = 2;
    int pointLight = 1;
    int spotLight = 1;
    int cube = 2;
    int sphere = 1;
    int cylinder = 1;
    int capsule = 1;
    int plane = 1;
    int empty = 1;
    int camera = 1;
    int import = 1;
};

class SceneSpawnService
{
public:
    void SetupDefaultSunLight(Scene& scene);
    void SetupObjects(Scene& scene);
    void ResetToDefault(Scene& scene);

    int AddObject(Scene& scene, ScenePrimitive primitive, int parentIndex);
    int AddEmptyObject(Scene& scene, int parentIndex);
    int AddLightObject(Scene& scene, LightType type, int parentIndex);
    int AddCameraObject(Scene& scene, int parentIndex);
    void EnsureUniqueMainCamera(Scene& scene, int objectIndex);

    int AllocateImportNumber();
    void ResetCounters();
    SceneSpawnCounters GetCounters() const;
    void SetCounters(const SceneSpawnCounters& counters);

private:
    int GetNextObjectNumber(ScenePrimitive primitive);

    int m_nextDirectionalLightNumber = 2;
    int m_nextPointLightNumber = 1;
    int m_nextSpotLightNumber = 1;
    int m_nextCubeNumber = 2;
    int m_nextSphereNumber = 1;
    int m_nextCylinderNumber = 1;
    int m_nextCapsuleNumber = 1;
    int m_nextPlaneNumber = 1;
    int m_nextEmptyNumber = 1;
    int m_nextCameraNumber = 1;
    int m_nextImportNumber = 1;
};
