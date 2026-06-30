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

struct GeometryLookupEntry
{
    uint vertexFloatOffset;
    uint vertexStrideFloats;
    uint indexUintOffset;
    uint _pad0;
};

StructuredBuffer<GeometryLookupEntry> g_GeometryLookup : register(t2);
StructuredBuffer<float> g_SceneVertexFloats : register(t3);
StructuredBuffer<uint> g_SceneIndices : register(t4);

// lib_6_3 does not declare DXR 1.1 ray flags; values match d3d12raytracing.h.
static const uint kRayFlagCullBackFacing = 0x20u;
static const uint kRayFlagCullFrontFacing = 0x40u;
static const uint kHitKindTriangleBackFace = 255u;

static const uint kRayFlagsBackCull =
    RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
    | RAY_FLAG_FORCE_OPAQUE
    | kRayFlagCullBackFacing;

static const uint kRayFlagsFrontCull =
    RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
    | RAY_FLAG_FORCE_OPAQUE
    | kRayFlagCullFrontFacing;

struct Payload
{
    float3 normal;
    float hitDistance;
    uint instanceId;
    uint primitiveIndex;
    uint hit;
    uint _pad;
};

float2 DepthUvToClipXY(float2 texCoord)
{
    return float2(texCoord.x * 2.0 - 1.0, 1.0 - texCoord.y * 2.0);
}

float3 LoadObjectPosition(GeometryLookupEntry geo, uint vertexIndex)
{
    const uint base = geo.vertexFloatOffset + vertexIndex * geo.vertexStrideFloats;
    return float3(
        g_SceneVertexFloats[base + 0],
        g_SceneVertexFloats[base + 1],
        g_SceneVertexFloats[base + 2]);
}

float3 ComputeWorldGeometricNormal(GeometryLookupEntry geo, uint primitiveIndex)
{
    const uint indexBase = geo.indexUintOffset + primitiveIndex * 3u;
    const uint i0 = g_SceneIndices[indexBase + 0];
    const uint i1 = g_SceneIndices[indexBase + 1];
    const uint i2 = g_SceneIndices[indexBase + 2];

    const float3 p0 = LoadObjectPosition(geo, i0);
    const float3 p1 = LoadObjectPosition(geo, i1);
    const float3 p2 = LoadObjectPosition(geo, i2);

    const float3x4 objectToWorld = ObjectToWorld3x4();
    const float3 w0 = mul(objectToWorld, float4(p0, 1.0)).xyz;
    const float3 w1 = mul(objectToWorld, float4(p1, 1.0)).xyz;
    const float3 w2 = mul(objectToWorld, float4(p2, 1.0)).xyz;

    return normalize(cross(w1 - w0, w2 - w0));
}

void ResetPayload(inout Payload payload)
{
    payload.normal = float3(0.0, 0.0, 1.0);
    payload.hitDistance = 0.0;
    payload.instanceId = 0;
    payload.primitiveIndex = 0;
    payload.hit = 0;
    payload._pad = 0;
}

float DeviceDepthFromWorld(float3 worldPos)
{
    const float4 clip = mul(g_ViewProj, float4(worldPos, 1.0));
    return clip.z / clip.w;
}

float DepthAgreementTolerance(const float bufferDepth)
{
    return max(5e-4, bufferDepth * 0.01);
}

bool AgreesWithDepthBuffer(const float hitDeviceDepth, const float bufferDepth)
{
    const float tolerance = DepthAgreementTolerance(bufferDepth);
    return abs(hitDeviceDepth - bufferDepth) <= tolerance;
}

bool IsInteriorDepthLeak(const float hitDeviceDepth, const float bufferDepth)
{
    const float tolerance = DepthAgreementTolerance(bufferDepth);
    return hitDeviceDepth < bufferDepth - tolerance;
}

bool IsBehindDepthSurface(const float hitDeviceDepth, const float bufferDepth)
{
    const float tolerance = DepthAgreementTolerance(bufferDepth);
    return hitDeviceDepth > bufferDepth + tolerance;
}

void ConsiderTraceCandidate(
    const Payload candidate,
    const float3 rayOrigin,
    const float3 rayDir,
    const float bufferDepth,
    const bool requireDepthAgreement,
    inout Payload bestPayload,
    inout float bestDepthError)
{
    if (candidate.hit == 0)
    {
        return;
    }

    const float3 hitWorld = rayOrigin + rayDir * candidate.hitDistance;
    const float hitDeviceDepth = DeviceDepthFromWorld(hitWorld);
    if (IsInteriorDepthLeak(hitDeviceDepth, bufferDepth))
    {
        return;
    }

    if (requireDepthAgreement)
    {
        if (!AgreesWithDepthBuffer(hitDeviceDepth, bufferDepth))
        {
            return;
        }
    }
    else if (IsBehindDepthSurface(hitDeviceDepth, bufferDepth))
    {
        return;
    }

    const float depthError = abs(hitDeviceDepth - bufferDepth);
    if (depthError < bestDepthError)
    {
        bestDepthError = depthError;
        bestPayload = candidate;
    }
}

void TraceCullPass(
    const RayDesc ray,
    const uint rayFlags,
    const float3 rayOrigin,
    const float3 rayDir,
    const float bufferDepth,
    const bool requireDepthAgreement,
    inout Payload bestPayload,
    inout float bestDepthError)
{
    Payload candidate;
    ResetPayload(candidate);
    TraceRay(g_SceneTlas, rayFlags, 0xFF, 0, 0, 0, ray, candidate);
    ConsiderTraceCandidate(
        candidate,
        rayOrigin,
        rayDir,
        bufferDepth,
        requireDepthAgreement,
        bestPayload,
        bestDepthError);
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
    float bestDepthError = 1e30;

    TraceCullPass(
        ray,
        kRayFlagsBackCull,
        ray.Origin,
        rayDir,
        bufferDepth,
        true,
        payload,
        bestDepthError);
    TraceCullPass(
        ray,
        kRayFlagsFrontCull,
        ray.Origin,
        rayDir,
        bufferDepth,
        true,
        payload,
        bestDepthError);

    if (payload.hit == 0)
    {
        bestDepthError = 1e30;
        TraceCullPass(
            ray,
            kRayFlagsBackCull,
            ray.Origin,
            rayDir,
            bufferDepth,
            false,
            payload,
            bestDepthError);
        TraceCullPass(
            ray,
            kRayFlagsFrontCull,
            ray.Origin,
            rayDir,
            bufferDepth,
            false,
            payload,
            bestDepthError);
    }

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
