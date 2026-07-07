// DXR path tracer — Phase P0 scaffolding (devdoc/dxr-path-tracing.md).
//
// This is the FOUNDATION of the unified megakernel path tracer, not the finished integrator. P0
// stands up the separate PT RTPSO/SBT and proves pure camera-ray tracing works with no raster
// dependency: it shoots one ray per pixel straight from the lens (NOT bounded by the depth buffer,
// unlike primary_debug.hlsl) and writes the primary-hit world normal as a debug visualization.
//
// It deliberately reuses the primary-debug global root signature + constants + output textures so P0
// adds no new dispatch/output plumbing — only a new shader library, pipeline, and SBT. P1 grows the
// raygen into the real path loop (direct lighting), P2 adds multi-bounce GI, etc. The binding layout
// (b0 + t0..t4 + u0/u1) matches DxrRootSignature::PrimaryDispatchConstants / the primary-debug root
// signature exactly; t1 (depth) is bound by the shared dispatch but intentionally unused here — a
// path tracer generates its own camera rays.

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

RWTexture2D<float4> g_Output : register(u0);   // rgb = world normal (or debug), a = hit distance
RWTexture2D<uint2> g_Metadata : register(u1);  // (instanceId+1, primitiveIndex) — reserved for blit

RaytracingAccelerationStructure g_SceneTlas : register(t0);
Texture2D<float> g_DepthMap : register(t1); // bound by the shared dispatch; UNUSED (pure camera rays)

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

static const uint kHitKindTriangleBackFace = 255u;
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

float2 PixelToClipXY(float2 texCoord)
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

[shader("raygeneration")]
void PathTracerRayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    if (pixel.x >= g_OutputSize.x || pixel.y >= g_OutputSize.y)
    {
        return;
    }

    // Pure path-tracing primary ray: reconstruct a world-space ray straight from the camera through
    // this pixel's centre. No depth-buffer read — the path tracer owns first-hit visibility (P1+ adds
    // sub-pixel jitter for anti-aliasing; P0 uses the centre so the normal image is stable).
    const float2 texCoord = (float2(pixel) + 0.5) / float2(g_OutputSize);
    const float2 clipXY = PixelToClipXY(texCoord);

    // Unproject a near and far clip point to define the ray direction robustly for any projection.
    const float4 farH = mul(g_InvViewProj, float4(clipXY, 1.0, 1.0));
    const float3 farWorld = farH.xyz / farH.w;
    const float3 rayDir = normalize(farWorld - g_CameraPos);

    RayDesc ray;
    ray.Origin = g_CameraPos;
    ray.Direction = rayDir;
    ray.TMin = 0.001;
    ray.TMax = max(g_MaxTraceDistance, g_FarPlane);

    Payload payload;
    ResetPayload(payload);
    TraceRay(g_SceneTlas, kPrimaryRayFlags, 0xFF, 0, 0, 0, ray, payload);

    if (payload.hit != 0)
    {
        // P0 debug output: RAW world normal + hit distance, matching primary_debug.hlsl so the shared
        // primary-debug blit (viewMode = normal) maps it to colour. P1 replaces this with HDR radiance.
        g_Output[pixel] = float4(payload.normal, payload.hitDistance);
        g_Metadata[pixel] = uint2(payload.instanceId + 1u, payload.primitiveIndex);
    }
    else
    {
        g_Output[pixel] = float4(0.0, 0.0, 0.0, 0.0);
        g_Metadata[pixel] = uint2(0, 0);
    }
}

[shader("miss")]
void PathTracerMiss(inout Payload payload)
{
    payload.hit = 0;
    payload.instanceId = 0;
    payload.primitiveIndex = 0;
    payload.hitDistance = 0.0;
    payload.normal = float3(0.0, 0.0, 1.0);
}

[shader("closesthit")]
void PathTracerClosestHit(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
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
