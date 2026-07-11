#include "hlsl_common.hlsl"

static const uint MAX_MESHLET_VERTICES = 64;
static const uint MAX_MESHLET_TRIANGLES = 64;

cbuffer MeshFrame : register(b0)
{
    float4x4 uModel;
    float4x4 uPrevModel;
    float4x4 uView;
    float4x4 uPrevView;
    float4x4 uProjection;
    float4x4 uUnjitteredProjection;
    float4x4 uPrevUnjitteredProjection;
    float4 uAlbedoRoughness;
    float4 uMetallicHistoryStride;
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
    float3 fragPos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 texCoord0 : TEXCOORD2;
    float2 texCoord1 : TEXCOORD3;
    float4 tangent : TEXCOORD4;
    float4 fragPosLightSpace : TEXCOORD5;
    float viewDepth : TEXCOORD6;
    float4 currClip : TEXCOORD7;
    float4 prevClip : TEXCOORD8;
    float4 albedoRoughness : TEXCOORD9;
    float metallic : TEXCOORD10;
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

    const uint stride = max(1u, (uint)round(uMetallicHistoryStride.z));

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

        float4 worldPos = mul(uModel, float4(position, 1.0));
        float4 viewPos = mul(uView, worldPos);

        float3x3 normalMatrix = NormalMatrixFromModel(uModel);
        float tangentHandedness = tangent.w;
        if (determinant((float3x3)uModel) < 0.0)
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
        if (uMetallicHistoryStride.y > 0.5)
        {
            float4 prevWorldPos = mul(uPrevModel, float4(position, 1.0));
            float4 prevViewPos = mul(uPrevView, prevWorldPos);
            output.prevClip = mul(uPrevUnjitteredProjection, prevViewPos);
        }
        else
        {
            output.prevClip = output.currClip;
        }
        output.albedoRoughness = uAlbedoRoughness;
        output.metallic = uMetallicHistoryStride.x;
        verts[groupThreadId] = output;
    }

    if (groupThreadId < meshlet.triangleCount)
    {
        const MeshletTriangle triRecord = gMeshletTriangles[meshlet.triangleOffset + groupThreadId];
        tris[groupThreadId] = uint3(triRecord.v0, triRecord.v1, triRecord.v2);
    }
}
