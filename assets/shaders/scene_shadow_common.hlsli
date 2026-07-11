#ifndef SCENE_SHADOW_COMMON_HLSLI
#define SCENE_SHADOW_COMMON_HLSLI

// Shared bindings/records for the depth-only shadow amplification + mesh shaders (C6). Mirrors
// scene_gbuffer_common.hlsli; the cbuffer carries the cascade's light matrix + world-space frustum
// planes instead of the camera matrices, and there is no material table (depth only).

#define SCENE_SHADOW_AS_GROUP_SIZE 32

cbuffer ShadowMeshFrame : register(b0)
{
    float4x4 uLightSpaceMatrix;
    float4 uLightDirectionBias; // xyz = light direction toward source, w = normal-offset depth bias
    float4 uCullParams;         // x = meshlet count, y = vertex float stride
    float4 uFrustumPlanes[6];   // world-space cascade frustum; inside when dot(xyz, p) + w >= 0
};

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

struct InstanceRecord
{
    float4x4 world;
    float4x4 prevWorld;
    uint meshId;
    uint materialId;
    uint flags;
    uint objectIndex;
    uint editorObjectIdLow;
    uint editorObjectIdHigh;
    uint pad0;
    uint pad1;
};

struct MeshletCullPayload
{
    uint instanceId;
    uint meshletIndices[SCENE_SHADOW_AS_GROUP_SIZE];
};

ByteAddressBuffer gVertexFloats : register(t0);
StructuredBuffer<MeshletRecord> gMeshlets : register(t1);
StructuredBuffer<uint> gMeshletVertices : register(t2);
StructuredBuffer<MeshletTriangle> gMeshletTriangles : register(t3);
StructuredBuffer<InstanceRecord> gInstances : register(t4);
StructuredBuffer<uint> gInstanceIds : register(t5);

#endif // SCENE_SHADOW_COMMON_HLSLI
