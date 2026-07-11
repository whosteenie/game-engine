#ifndef SCENE_GBUFFER_COMMON_HLSLI
#define SCENE_GBUFFER_COMMON_HLSLI

// Shared bindings/records for the scene G-buffer amplification + mesh shaders (C5 path, C5.5 culling).
// One header keeps the cbuffer layout, the meshlet/instance records, and the AS->MS payload identical
// across both stages.

// Amplification-shader group size: one group culls this many meshlets of one instance and forwards
// the survivors to the mesh shader. Must equal kMeshShaderAsGroupSize on the C++ side. 32 keeps a
// group within a single hardware wave on both NVIDIA (32) and AMD (64), so the wave-intrinsic
// compaction below is group-wide without a groupshared prefix scan.
#define SCENE_GBUFFER_AS_GROUP_SIZE 32

cbuffer MeshFrame : register(b0)
{
    float4x4 uView;
    float4x4 uPrevView;
    float4x4 uProjection;
    float4x4 uUnjitteredProjection;
    float4x4 uPrevUnjitteredProjection;
    float4 uHistoryStrideMeshId; // x = temporal history valid, y = float stride, z = meshId
    float4 uCullParams;          // x = meshlet count for this batch
    float4 uFrustumPlanes[6];    // world-space; inside when dot(xyz, p) + w >= 0
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

// AS -> MS payload: one instance's surviving meshlet indices, compacted by the amplification shader.
struct MeshletCullPayload
{
    uint instanceId;
    uint meshletIndices[SCENE_GBUFFER_AS_GROUP_SIZE];
};

ByteAddressBuffer gVertexFloats : register(t0);
StructuredBuffer<MeshletRecord> gMeshlets : register(t1);
StructuredBuffer<uint> gMeshletVertices : register(t2);
StructuredBuffer<MeshletTriangle> gMeshletTriangles : register(t3);
StructuredBuffer<InstanceRecord> gInstances : register(t4);
StructuredBuffer<uint> gInstanceIds : register(t6);

#endif // SCENE_GBUFFER_COMMON_HLSLI
