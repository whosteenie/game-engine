// DXR Phase D4 — specular reflection trace (see devdoc/dxr-reflections.md).
// Output: RGBA16F radiance (rgb) + confidence (a) at quality-scaled resolution.
// Hit shading v1: screen-radiance reuse (RT0 + RT3*shadow + RT1) when the hit point is
// visible on screen; env-cube approximation otherwise. Miss: prefiltered env, confidence 0.

cbuffer ReflectionDispatchConstants : register(b0)
{
    uint2 g_OutputSize;
    uint2 g_GBufferSize;
    float4x4 g_InvViewProj; // jittered, matches the depth buffer
    float4x4 g_ViewProj;    // jittered, for hit -> screen reprojection
    float3 g_CameraPos;
    float g_MaxTraceDistance;
    float g_EnvironmentIntensity;
    float g_MaxReflectionLod;
    uint g_FrameIndex;
    uint g_SamplesPerPixel;
};

RWTexture2D<float4> g_ReflectionOutput : register(u0);

RaytracingAccelerationStructure g_SceneTlas : register(t0);
Texture2D<float> g_DepthMap : register(t1);
Texture2D<float4> g_NormalMap : register(t2);    // shading normal (RT2)
Texture2D<float4> g_Material0Map : register(t3); // albedo.rgb + roughness.a (RT5)

struct GeometryLookupEntry
{
    uint vertexFloatOffset;
    uint vertexStrideFloats;
    uint indexUintOffset;
    uint _pad0;
};

StructuredBuffer<GeometryLookupEntry> g_GeometryLookup : register(t4);
StructuredBuffer<float> g_SceneVertexFloats : register(t5);
StructuredBuffer<uint> g_SceneIndices : register(t6);

Texture2D<float4> g_DirectMap : register(t7);    // RT0 fill direct + emissive
Texture2D<float4> g_SunShadowMap : register(t8); // RT3 sun rgb + shadow factor a
Texture2D<float4> g_IndirectMap : register(t9);  // RT1 indirect/ambient
TextureCube<float4> g_PrefilterMap : register(t10);

SamplerState g_LinearClampSampler : register(s0);

static const float kPi = 3.14159265;
static const uint kHitKindTriangleBackFace = 255u;
static const uint kReflectionRayFlags = RAY_FLAG_FORCE_OPAQUE;
static const float kMaxRadiance = 64.0;

struct Payload
{
    float3 radiance;
    float confidence;
    float hitDistance;
    float surfaceRoughness; // in: receiver roughness (for miss mip selection)
    uint hit;
    uint _pad;
};

float2 DepthUvToClipXY(float2 texCoord)
{
    return float2(texCoord.x * 2.0 - 1.0, 1.0 - texCoord.y * 2.0);
}

float2 ClipXYToDepthUv(float2 clipXY)
{
    return float2(clipXY.x * 0.5 + 0.5, (1.0 - clipXY.y) * 0.5);
}

float Hash1(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 34.345);
    return frac(p.x * p.y);
}

float2 Hash2(float2 p)
{
    return float2(Hash1(p), Hash1(p.yx + 19.19));
}

void BuildTangentFrame(float3 normal, out float3 tangent, out float3 bitangent)
{
    const float3 up = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    tangent = normalize(cross(up, normal));
    bitangent = cross(normal, tangent);
}

// GGX NDF half-vector importance sample (same math as ssr_trace.ps.hlsl).
float3 SampleGgxHalfVector(float3 normal, float roughness, float2 xi)
{
    const float alpha = max(roughness * roughness, 1e-3);
    const float phi = 2.0 * kPi * xi.x;
    const float cosTheta = sqrt(saturate((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y)));
    const float sinTheta = sqrt(saturate(1.0 - cosTheta * cosTheta));

    float3 tangent;
    float3 bitangent;
    BuildTangentFrame(normal, tangent, bitangent);

    return normalize(
        tangent * (sinTheta * cos(phi))
        + bitangent * (sinTheta * sin(phi))
        + normal * cosTheta);
}

float3 ClampRadiance(float3 radiance)
{
    radiance = max(radiance, 0.0.xxx);
    const float luminance = dot(radiance, float3(0.2126, 0.7152, 0.0722));
    if (luminance <= kMaxRadiance)
    {
        return radiance;
    }

    return radiance * (kMaxRadiance / max(luminance, 1e-4));
}

float3 SampleEnvironment(float3 direction, float roughness)
{
    return g_PrefilterMap.SampleLevel(
        g_LinearClampSampler,
        direction,
        roughness * g_MaxReflectionLod).rgb * g_EnvironmentIntensity;
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

[shader("raygeneration")]
void ReflectionRayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    if (pixel.x >= g_OutputSize.x || pixel.y >= g_OutputSize.y)
    {
        return;
    }

    const float2 uv = (float2(pixel) + 0.5) / float2(g_OutputSize);
    const int2 gbufferPixel = int2(uv * float2(g_GBufferSize));
    const float depth = g_DepthMap.Load(int3(gbufferPixel, 0)).r;

    // Sky: show env in the raw view, confidence 0 so composite/denoise ignore it.
    const float2 clipXY = DepthUvToClipXY(uv);
    if (depth >= 0.9999)
    {
        const float4 farH = mul(g_InvViewProj, float4(clipXY, 1.0, 1.0));
        const float3 viewDir = normalize(farH.xyz / farH.w - g_CameraPos);
        g_ReflectionOutput[pixel] = float4(SampleEnvironment(viewDir, 0.0), 0.0);
        return;
    }

    const float4 worldH = mul(g_InvViewProj, float4(clipXY, depth, 1.0));
    const float3 worldPos = worldH.xyz / worldH.w;
    const float3 shadingNormal = normalize(g_NormalMap.Load(int3(gbufferPixel, 0)).xyz);
    const float roughness = g_Material0Map.Load(int3(gbufferPixel, 0)).a;

    const float3 viewVec = normalize(worldPos - g_CameraPos); // camera -> surface
    const float3 mirrorDir = reflect(viewVec, shadingNormal);

    const float surfaceDistance = length(worldPos - g_CameraPos);
    const float3 rayOrigin =
        worldPos + shadingNormal * max(surfaceDistance * 0.0015, 0.01);

    const uint sampleCount = clamp(g_SamplesPerPixel, 1u, 4u);
    float3 radianceSum = 0.0.xxx;
    float confidenceSum = 0.0;
    float weightSum = 0.0;

    [loop]
    for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
    {
        const float2 xi = Hash2(
            uv * 913.7
            + float2(
                (float)g_FrameIndex * 0.6180 + (float)sampleIndex * 2.71,
                (float)g_FrameIndex * 0.7548 + (float)sampleIndex * 5.13));

        float3 rayDir = mirrorDir;
        if (roughness > 0.03)
        {
            const float3 halfVector = SampleGgxHalfVector(shadingNormal, roughness, xi);
            const float3 sampled = reflect(viewVec, halfVector);
            if (dot(sampled, shadingNormal) > 1e-4)
            {
                rayDir = normalize(sampled);
            }
        }

        RayDesc ray;
        ray.Origin = rayOrigin;
        ray.Direction = rayDir;
        ray.TMin = 0.001;
        ray.TMax = max(g_MaxTraceDistance, 0.1);

        Payload payload;
        payload.radiance = 0.0.xxx;
        payload.confidence = 0.0;
        payload.hitDistance = 0.0;
        payload.surfaceRoughness = roughness;
        payload.hit = 0;
        payload._pad = 0;

        TraceRay(g_SceneTlas, kReflectionRayFlags, 0xFF, 0, 0, 0, ray, payload);

        radianceSum += ClampRadiance(payload.radiance);
        confidenceSum += payload.confidence;
        weightSum += 1.0;
    }

    const float invSamples = 1.0 / max(weightSum, 1.0);
    g_ReflectionOutput[pixel] =
        float4(radianceSum * invSamples, saturate(confidenceSum * invSamples));
}

[shader("miss")]
void ReflectionMiss(inout Payload payload)
{
    // Ray left the scene: prefiltered environment IS the correct radiance, but confidence 0
    // tells the D6 composite to keep its own (BRDF-weighted) IBL term instead.
    payload.radiance = SampleEnvironment(WorldRayDirection(), payload.surfaceRoughness);
    payload.confidence = 0.0;
    payload.hitDistance = 0.0;
    payload.hit = 0;
}

[shader("closesthit")]
void ReflectionClosestHit(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    (void)attribs;

    const uint instanceId = InstanceID();
    const uint primitiveIndex = PrimitiveIndex();
    const GeometryLookupEntry geo = g_GeometryLookup[instanceId];

    const float3 rayDir = WorldRayDirection();
    const float hitT = RayTCurrent();
    const float3 hitWorld = WorldRayOrigin() + rayDir * hitT;

    float3 hitNormal = ComputeWorldGeometricNormal(geo, primitiveIndex);
    if (HitKind() == kHitKindTriangleBackFace)
    {
        hitNormal = -hitNormal;
    }
    if (dot(hitNormal, rayDir) > 0.0)
    {
        hitNormal = -hitNormal;
    }

    payload.hit = 1;
    payload.hitDistance = hitT;

    // Reproject the hit point into the current frame and reuse screen radiance when valid
    // (RT0 + RT3*shadow + RT1 — same energy assembly as the SSR scene color, read directly
    // from the MRTs; no SSR buffers involved).
    const float4 hitClip = mul(g_ViewProj, float4(hitWorld, 1.0));
    bool screenValid = hitClip.w > 1e-4;
    float2 hitUv = 0.0.xx;
    if (screenValid)
    {
        const float2 hitClipXY = hitClip.xy / hitClip.w;
        hitUv = ClipXYToDepthUv(hitClipXY);
        screenValid = hitUv.x >= 0.0 && hitUv.x <= 1.0 && hitUv.y >= 0.0 && hitUv.y <= 1.0;
    }

    if (screenValid)
    {
        const float hitDeviceDepth = hitClip.z / hitClip.w;
        const int2 gbufferPixel = int2(hitUv * float2(g_GBufferSize));
        const float storedDepth = g_DepthMap.Load(int3(gbufferPixel, 0)).r;
        const float tolerance = max(5e-4, storedDepth * 0.01);
        screenValid = abs(hitDeviceDepth - storedDepth) <= tolerance;
    }

    if (screenValid)
    {
        const float3 fillAndEmissive =
            g_DirectMap.SampleLevel(g_LinearClampSampler, hitUv, 0.0).rgb;
        const float4 sunShadow =
            g_SunShadowMap.SampleLevel(g_LinearClampSampler, hitUv, 0.0);
        const float3 indirect =
            g_IndirectMap.SampleLevel(g_LinearClampSampler, hitUv, 0.0).rgb;

        payload.radiance =
            max(fillAndEmissive + sunShadow.rgb * sunShadow.a + indirect, 0.0.xxx);

        const float distance01 = saturate(hitT / max(g_MaxTraceDistance, 1e-4));
        const float distanceWeight = 1.0 - distance01 * distance01;
        const float facingWeight = saturate(dot(hitNormal, -rayDir));
        payload.confidence = distanceWeight * facingWeight;
        return;
    }

    // Off-screen / occluded hit: no per-instance material table yet (see spec). Ambient
    // approximation — env cube along the hit normal at max mip, mid-gray albedo guess.
    payload.radiance = SampleEnvironment(hitNormal, 1.0) * 0.5;
    payload.confidence = 0.25;
}
