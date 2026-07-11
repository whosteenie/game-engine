#include "scene_gbuffer_common.hlsli"
#include "meshlet_cull.hlsli"

// Amplification (task) shader: per (meshlet-group, instance) it frustum-culls each meshlet's
// world-space bounding sphere and launches mesh-shader groups only for the survivors — so mesh-shader
// work scales with VISIBLE meshlets, not total meshlets x instances (C5.5). Dispatched as
// DispatchMesh(ceil(meshletCount / GROUP), instanceCount, 1): groupId.x = meshlet group,
// groupId.y = index into the batch's instance-id list.

groupshared MeshletCullPayload s_payload;

[numthreads(SCENE_GBUFFER_AS_GROUP_SIZE, 1, 1)]
void main(uint groupThreadId : SV_GroupThreadID, uint3 groupId : SV_GroupID)
{
    const uint meshletCount = (uint)round(uCullParams.x);
    const uint instanceId = gInstanceIds[groupId.y];
    const uint meshletIndex = groupId.x * SCENE_GBUFFER_AS_GROUP_SIZE + groupThreadId;

    bool visible = false;
    if (meshletIndex < meshletCount)
    {
        const MeshletRecord meshlet = gMeshlets[meshletIndex];
        float3 worldCenter;
        float worldRadius;
        MeshletWorldBoundingSphere(
            meshlet.boundsCenter,
            meshlet.boundsRadius,
            gInstances[instanceId].world,
            worldCenter,
            worldRadius);
        visible = SphereInsideFrustum(uFrustumPlanes, worldCenter, worldRadius);
    }

    if (groupThreadId == 0)
    {
        s_payload.instanceId = instanceId;
    }

    // Compact survivors into contiguous payload slots (single-wave group -> wave intrinsics are
    // group-wide).
    if (visible)
    {
        const uint slot = WavePrefixCountBits(visible);
        s_payload.meshletIndices[slot] = meshletIndex;
    }
    const uint visibleCount = WaveActiveCountBits(visible);

    // Ensure all payload writes are visible before handing the payload to the mesh shader.
    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(visibleCount, 1, 1, s_payload);
}
