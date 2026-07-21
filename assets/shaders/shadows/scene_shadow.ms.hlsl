#include "../common/hlsl_common.hlsl"
#include "scene_shadow_common.hlsli"

static const uint MAX_MESHLET_VERTICES = 64;
static const uint MAX_MESHLET_TRIANGLES = 64;

struct VertexOut
{
    float4 position : SV_Position;
};

float LoadFloat(uint floatIndex)
{
    return asfloat(gVertexFloats.Load(floatIndex * 4));
}

[numthreads(128, 1, 1)]
[outputtopology("triangle")]
void main(
    uint groupThreadId : SV_GroupThreadID,
    uint3 groupId : SV_GroupID,
    in payload MeshletCullPayload payloadData,
    out vertices VertexOut verts[MAX_MESHLET_VERTICES],
    out indices uint3 tris[MAX_MESHLET_TRIANGLES])
{
    const uint meshletIndex = payloadData.meshletIndices[groupId.x];
    const MeshletRecord meshlet = gMeshlets[meshletIndex];
    SetMeshOutputCounts(meshlet.vertexCount, meshlet.triangleCount);

    const InstanceRecord instance = gInstances[payloadData.instanceId];
    const uint stride = max(1u, (uint)round(uCullParams.y));

    if (groupThreadId < meshlet.vertexCount)
    {
        const uint globalVertex = gMeshletVertices[meshlet.vertexOffset + groupThreadId];
        const uint baseFloat = globalVertex * stride;

        const float3 position = float3(
            LoadFloat(baseFloat + 0),
            LoadFloat(baseFloat + 1),
            LoadFloat(baseFloat + 2));
        const float3 normal = float3(
            LoadFloat(baseFloat + 3),
            LoadFloat(baseFloat + 4),
            LoadFloat(baseFloat + 5));

        float4 clip = mul(uLightSpaceMatrix, mul(instance.world, float4(position, 1.0)));

        float3 normalWorld = normalize(mul((float3x3)instance.world, normal));
        float nDotL = dot(normalWorld, normalize(uLightDirectionBias.xyz));
        float sinTheta = (nDotL > 0.0) ? sqrt(saturate(1.0 - nDotL * nDotL)) : 0.0;
        clip.z += uLightDirectionBias.w * sinTheta * clip.w;

        VertexOut output;
        output.position = clip;
        verts[groupThreadId] = output;
    }

    if (groupThreadId < meshlet.triangleCount)
    {
        const MeshletTriangle triRecord = gMeshletTriangles[meshlet.triangleOffset + groupThreadId];
        tris[groupThreadId] = uint3(triRecord.v0, triRecord.v1, triRecord.v2);
    }
}
