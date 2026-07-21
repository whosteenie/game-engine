#include "../common/hlsl_common.hlsl"
#include "scene_gbuffer_common.hlsli"

static const uint MAX_MESHLET_VERTICES = 64;
static const uint MAX_MESHLET_TRIANGLES = 64;

struct VertexOut
{
    float4 position : SV_Position;
    float3 fragPos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 texCoord0 : TEXCOORD2;
    float2 texCoord1 : TEXCOORD3;
    float4 tangent : TEXCOORD4;
    float4 fragPosLightSpace : TEXCOORD5;
    float viewDepth : TEXCOORD6;
    float4 currClip : TEXCOORD7;
    float4 prevClip : TEXCOORD8;
    uint instanceId : TEXCOORD9;
    uint materialId : TEXCOORD10;
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
    // The amplification shader already culled and compacted: groupId.x indexes the surviving meshlets
    // it forwarded for this instance.
    const uint meshletIndex = payloadData.meshletIndices[groupId.x];
    const MeshletRecord meshlet = gMeshlets[meshletIndex];
    SetMeshOutputCounts(meshlet.vertexCount, meshlet.triangleCount);

    const uint instanceId = payloadData.instanceId;
    const InstanceRecord instance = gInstances[instanceId];
    const uint stride = max(1u, (uint)round(uHistoryStrideMeshId.y));

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

        float2 texCoord0 = 0.0.xx;
        float2 texCoord1 = 0.0.xx;
        float4 tangent = float4(1.0, 0.0, 0.0, 1.0);
        if (stride >= 8)
        {
            texCoord0 = float2(LoadFloat(baseFloat + 6), LoadFloat(baseFloat + 7));
        }
        if (stride >= 10)
        {
            texCoord1 = float2(LoadFloat(baseFloat + 8), LoadFloat(baseFloat + 9));
        }
        if (stride >= 14)
        {
            tangent = float4(
                LoadFloat(baseFloat + 10),
                LoadFloat(baseFloat + 11),
                LoadFloat(baseFloat + 12),
                LoadFloat(baseFloat + 13));
        }

        float4 worldPos = mul(instance.world, float4(position, 1.0));
        float4 viewPos = mul(uView, worldPos);

        float3x3 normalMatrix = NormalMatrixFromModel(instance.world);
        float tangentHandedness = tangent.w;
        if (determinant((float3x3)instance.world) < 0.0)
        {
            tangentHandedness = -tangentHandedness;
        }

        VertexOut output;
        output.fragPos = worldPos.xyz;
        output.normal = mul(normalMatrix, normal);
        output.texCoord0 = texCoord0;
        output.texCoord1 = texCoord1;
        output.tangent = float4(normalize(mul(normalMatrix, tangent.xyz)), tangentHandedness);
        output.fragPosLightSpace = 0.0.xxxx;
        output.viewDepth = viewPos.z;
        output.position = mul(uProjection, viewPos);
        output.currClip = mul(uUnjitteredProjection, viewPos);
        if (uHistoryStrideMeshId.x > 0.5)
        {
            float4 prevWorldPos = mul(instance.prevWorld, float4(position, 1.0));
            float4 prevViewPos = mul(uPrevView, prevWorldPos);
            output.prevClip = mul(uPrevUnjitteredProjection, prevViewPos);
        }
        else
        {
            output.prevClip = output.currClip;
        }
        output.instanceId = instanceId;
        output.materialId = instance.materialId;
        verts[groupThreadId] = output;
    }

    if (groupThreadId < meshlet.triangleCount)
    {
        const MeshletTriangle triRecord = gMeshletTriangles[meshlet.triangleOffset + groupThreadId];
        tris[groupThreadId] = uint3(triRecord.v0, triRecord.v1, triRecord.v2);
    }
}
