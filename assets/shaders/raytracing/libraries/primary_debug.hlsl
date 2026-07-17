cbuffer PrimaryDispatchConstants : register(b0)
{
    uint2 g_OutputSize;
    uint2 _Padding0;
    float4x4 g_InvViewProj;
    float4x4 g_ViewProj;
    float3 g_CameraPos;
    float _Padding1;
    float g_NearPlane;
    float g_FarPlane;
    float g_MaxTraceDistance;
    float _Padding2;
};

RWTexture2D<float4> g_PrimaryOutput : register(u0);
RWTexture2D<uint2> g_PrimaryMetadata : register(u1);

RaytracingAccelerationStructure g_SceneTlas : register(t0);
Texture2D<float> g_DepthMap : register(t1);

#include "../common/dxr_geometry_types.hlsli"

StructuredBuffer<GeometryLookupEntry> g_GeometryLookup : register(t2);
StructuredBuffer<float> g_SceneVertexFloats : register(t3);
StructuredBuffer<uint> g_SceneIndices : register(t4);

#include "../common/dxr_geometry.hlsli"

// DXR-04: primary visibility must return the CLOSEST hit. The previous implementation used
// RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH (which returns *any* hit, not the closest) and then
// rejected hits that disagreed with the depth buffer, producing speckle/holes and requiring a
// 4-rays-per-pixel front/back-cull cascade as a workaround. One closest-hit trace with a
// bounded TMax replaces all of that. No culling: backface hits are valid primary visibility
// (the normal is flipped toward the ray in the closest-hit shader).
static const uint kPrimaryRayFlags = RAY_FLAG_FORCE_OPAQUE;

struct Payload
{
    float3 normal;
    float hitDistance;
    uint instanceId;
    uint primitiveIndex;
    uint hit;
    uint _pad;
};

void ResetPayload(inout Payload payload)
{
    payload.normal = float3(0.0, 0.0, 1.0);
    payload.hitDistance = 0.0;
    payload.instanceId = 0;
    payload.primitiveIndex = 0;
    payload.hit = 0;
    payload._pad = 0;
}

[shader("raygeneration")]
void PrimaryRayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    if (pixel.x >= g_OutputSize.x || pixel.y >= g_OutputSize.y)
    {
        return;
    }

    const float2 texCoord = (float2(pixel) + 0.5) / float2(g_OutputSize);
    const float bufferDepth = g_DepthMap.Load(int3(pixel, 0)).r;
    if (bufferDepth >= 0.99999)
    {
        g_PrimaryOutput[pixel] = float4(0.0, 0.0, 1.0, 0.0);
        g_PrimaryMetadata[pixel] = uint2(0, 0);
        return;
    }

    const float2 clipXY = DepthUvToClipXY(texCoord);
    const float4 worldH = mul(g_InvViewProj, float4(clipXY, bufferDepth, 1.0));
    const float3 worldPos = worldH.xyz / worldH.w;
    const float3 rayDir = normalize(worldPos - g_CameraPos);
    const float depthRayT = length(worldPos - g_CameraPos);

    RayDesc ray;
    ray.Origin = g_CameraPos;
    ray.Direction = rayDir;
    ray.TMin = 0.001;
    ray.TMax = depthRayT + max(0.05, depthRayT * 0.015);

    Payload payload;
    ResetPayload(payload);
    TraceRay(g_SceneTlas, kPrimaryRayFlags, 0xFF, 0, 0, 0, ray, payload);

    if (payload.hit != 0)
    {
        g_PrimaryOutput[pixel] = float4(payload.normal, payload.hitDistance);
        // instanceId+1 so 0 remains reserved for miss metadata in the debug blit.
        g_PrimaryMetadata[pixel] = uint2(payload.instanceId + 1u, payload.primitiveIndex);
    }
    else
    {
        g_PrimaryOutput[pixel] = float4(0.0, 0.0, 1.0, 0.0);
        g_PrimaryMetadata[pixel] = uint2(0, 0);
    }
}

[shader("miss")]
void PrimaryMiss(inout Payload payload)
{
    payload.hit = 0;
    payload.instanceId = 0;
    payload.primitiveIndex = 0;
    payload.hitDistance = 0.0;
    payload.normal = float3(0.0, 0.0, 1.0);
}

[shader("closesthit")]
void PrimaryClosestHit(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    (void)attribs;

    const uint instanceId = InstanceID();
    const uint primitiveIndex = PrimitiveIndex();
    const GeometryLookupEntry geo = g_GeometryLookup[instanceId];

    payload.hit = 1;
    payload.instanceId = instanceId;
    payload.primitiveIndex = primitiveIndex;
    payload.hitDistance = RayTCurrent();
    float3 normal = ComputeWorldGeometricNormal(geo, primitiveIndex);
    if (HitKind() == kHitKindTriangleBackFace)
    {
        normal = -normal;
    }

    const float3 rayDir = WorldRayDirection();
    if (dot(normal, rayDir) > 0.0)
    {
        normal = -normal;
    }

    payload.normal = normal;
}
