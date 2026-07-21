#include "scene_shadow_common.hlsli"
#include "../geometry/meshlet_cull.hlsli"

// Shadow amplification shader (C6): identical structure to the G-buffer AS, but culls meshlets
// against the CASCADE frustum (supplied in uFrustumPlanes) instead of the camera frustum. A meshlet
// outside the cascade's ortho volume would be clipped by the rasterizer anyway, so this is lossless.

groupshared MeshletCullPayload s_payload;

[numthreads(SCENE_SHADOW_AS_GROUP_SIZE, 1, 1)]
void main(uint groupThreadId : SV_GroupThreadID, uint3 groupId : SV_GroupID)
{
    const uint meshletCount = (uint)round(uCullParams.x);
    const uint instanceId = gInstanceIds[groupId.y];
    const uint meshletIndex = groupId.x * SCENE_SHADOW_AS_GROUP_SIZE + groupThreadId;

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
    if (visible)
    {
        const uint slot = WavePrefixCountBits(visible);
        s_payload.meshletIndices[slot] = meshletIndex;
    }
    const uint visibleCount = WaveActiveCountBits(visible);

    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(visibleCount, 1, 1, s_payload);
}
