#include "hlsl_common.hlsl"

static const uint MAX_MESHLET_VERTICES = 64;
static const uint MAX_MESHLET_TRIANGLES = 64;

cbuffer ShadowMeshFrame : register(b0)
{
    float4x4 uModel;
    float4x4 uLightSpaceMatrix;
    float4 uLightDirectionBias;
    float4 uVertexStridePad;
};

ByteAddressBuffer gVertexFloats : register(t0);

struct MeshletRecord
{
    uint vertexOffset;
    uint vertexCount;
    uint triangleOffset;
    uint triangleCount;
    float3 boundsCenter;
    float boundsRadius;
    float3 boundsMin;
    uint flags;
    float3 boundsMax;
    uint pad0;
};

struct MeshletTriangle
{
    uint v0;
    uint v1;
    uint v2;
    uint pad0;
};

StructuredBuffer<MeshletRecord> gMeshlets : register(t1);
StructuredBuffer<uint> gMeshletVertices : register(t2);
StructuredBuffer<MeshletTriangle> gMeshletTriangles : register(t3);

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
    uint groupId : SV_GroupID,
    out vertices VertexOut verts[MAX_MESHLET_VERTICES],
    out indices uint3 tris[MAX_MESHLET_TRIANGLES])
{
    const MeshletRecord meshlet = gMeshlets[groupId];
    SetMeshOutputCounts(meshlet.vertexCount, meshlet.triangleCount);

    const uint stride = max(1u, (uint)round(uVertexStridePad.x));

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

        float4 clip = mul(uLightSpaceMatrix, mul(uModel, float4(position, 1.0)));

        float3 normalWorld = normalize(mul((float3x3)uModel, normal));
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
