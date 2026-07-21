#pragma once

#include "engine/rendering/scene/GpuScene.h"

class Scene;

// Converts editable scene state into renderer-owned GPU scene tables. Keeping this adapter in
// app prevents engine rendering and ray tracing code from depending on editor scene types.
class GpuSceneBuilder
{
public:
    static void Build(
        GpuScene& gpuScene,
        const Scene& scene,
        const GpuScene::PreviousWorldMap& previousWorldByObjectId);
};

std::uint32_t CountSelectedRenderInstances(const GpuScene& gpuScene, const Scene& scene);
const GpuSceneInstanceRecord* FindPrimarySelectionInstance(const GpuScene& gpuScene, const Scene& scene);
