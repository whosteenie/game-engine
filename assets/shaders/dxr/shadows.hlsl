// DXR Phase D8 — ray-traced soft directional (sun) shadows + NRD SIGMA_SHADOW guides
// (see devdoc/dxr-shadows.md). Runs at FULL render resolution (1 spp; SIGMA is built for 1 spp).
// u0: R16F packed penumbra  — SIGMA_FrontEnd_PackPenumbra(distanceToOccluder, tanAngularRadius);
//     miss (no occluder = lit) packs NRD_FP16_MAX.
// u1: R32F linear viewZ      — SIGMA guide (sky = large value beyond the denoising range)
// u2: RGBA16_UNORM normal+roughness — MUST match NRD_NORMAL_ENCODING=3 / NRD_ROUGHNESS_ENCODING=1
//     (rgb = worldNormal*0.5+0.5, a = linear roughness). Same contract as reflections.hlsl.
// u3: RG16F screen-space motion — NRD convention mv = uvPrev - uvCurr (motionVectorScale {1,1}).
// The closest-hit shader is present but empty apart from writing RayTCurrent(): SIGMA needs the
// hit distance for the penumbra pack, so we do NOT use RAY_FLAG_SKIP_CLOSEST_HIT_SHADER
// (dxr-shadows.md "The shadow ray", option (a)).

cbuffer ShadowDispatchConstants : register(b0)
{
    uint2 g_OutputSize;
    uint2 g_GBufferSize;      // full render resolution == OutputSize
    float4x4 g_InvViewProj;   // jittered, matches the depth buffer
    float4x4 g_WorldToView;   // for linear viewZ (NRD guide)
    float3 g_CameraPos;
    float g_SunAngularTanRadius; // tan(radians(sunAngularRadiusDegrees))
    float3 g_SunDirection;       // TOWARD the light (normalized)
    float g_MaxTraceDistance;
    uint g_FrameIndex;
    uint _pad0;
    uint _pad1;
    uint _pad2;
};

RWTexture2D<float> g_PenumbraOutput : register(u0);
RWTexture2D<float> g_ViewZOutput : register(u1);
RWTexture2D<float4> g_NormalRoughnessOutput : register(u2);
RWTexture2D<float2> g_MotionOutput : register(u3);

RaytracingAccelerationStructure g_SceneTlas : register(t0);
Texture2D<float> g_DepthMap : register(t1);
Texture2D<float4> g_NormalMap : register(t2);    // shading normal (RT2)
Texture2D<float4> g_Material0Map : register(t3); // albedo.rgb + roughness.a (RT5)
Texture2D<float4> g_VelocityMap : register(t4);  // RT4 motion NDC (curr - prev)

static const float kPi = 3.14159265;
// From NRD.hlsli (v4.17.3) — must fit into FP16. Copied verbatim (packing constants).
static const float NRD_FP16_MAX = 65504.0;

// Occlusion query only needs the FIRST blocker; the closest hit is irrelevant. Note this flag
// was a BUG for primary visibility (DXR-04) but is CORRECT here — see dxr-shadows.md.
static const uint kShadowRayFlags =
    RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE;

struct ShadowPayload
{
    float hitT; // sentinel until a hit/miss shader writes it
};

// ---- Copied verbatim from NRD.hlsli (NVIDIA-RTX/NRD v4.17.3, Shaders/NRD.hlsli). ----
// (c) NVIDIA CORPORATION. Licensed per the NRD repository license. Infinite (directional) light.
// X => IN_PENUMBRA
float SIGMA_FrontEnd_PackPenumbra(float distanceToOccluder, float tanOfLightAngularRadius)
{
    float penumbraSize = distanceToOccluder * tanOfLightAngularRadius;
    float penumbraRadius = penumbraSize * 0.5;

    return distanceToOccluder >= NRD_FP16_MAX ? NRD_FP16_MAX : min(penumbraRadius, 32768.0);
}
// ---- end verbatim ----

float2 DepthUvToClipXY(float2 texCoord)
{
    return float2(texCoord.x * 2.0 - 1.0, 1.0 - texCoord.y * 2.0);
}

// Integer PCG hash (pcg3d, Jarzynski & Olano) — copied from reflections.hlsl. A static per-pixel
// sequence makes the temporal denoiser converge TO the noise (RTQ-01 / dxr-shadows.md DO-NOT #1).
uint3 Pcg3d(uint3 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

// Four decorrelated randoms per sample (only .xy used for the cone disk here). [0; 1).
float4 RandomXi4(uint2 pixel, uint frameIndex, uint sampleIndex)
{
    const uint3 hashA = Pcg3d(uint3(pixel.x, pixel.y, frameIndex * 64u + sampleIndex));
    const uint3 hashB = Pcg3d(uint3(pixel.y ^ 0x9E3779B9u, pixel.x, frameIndex * 64u + sampleIndex + 32u));
    return float4(
        float2(hashA.xy & 0x00FFFFFFu) * (1.0 / 16777216.0),
        float2(hashB.xy & 0x00FFFFFFu) * (1.0 / 16777216.0));
}

void BuildTangentFrame(float3 normal, out float3 tangent, out float3 bitangent)
{
    const float3 up = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    tangent = normalize(cross(up, normal));
    bitangent = cross(normal, tangent);
}

[shader("raygeneration")]
void ShadowRayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    if (pixel.x >= g_OutputSize.x || pixel.y >= g_OutputSize.y)
    {
        return;
    }

    const float2 uv = (float2(pixel) + 0.5) / float2(g_OutputSize);
    const int2 gbufferPixel = int2(uv * float2(g_GBufferSize));
    const float depth = g_DepthMap.Load(int3(gbufferPixel, 0)).r;
    const float2 clipXY = DepthUvToClipXY(uv);

    // Sky: no surface, must read as fully LIT (penumbra = NRD_FP16_MAX). DO NOT zero background
    // (dxr-shadows.md DO-NOT #2) — a 0 here would paint the sky black in the composite.
    if (depth >= 0.9999)
    {
        g_PenumbraOutput[pixel] = NRD_FP16_MAX;
        g_ViewZOutput[pixel] = 1e6;
        g_NormalRoughnessOutput[pixel] = float4(0.5, 0.5, 1.0, 1.0);
        g_MotionOutput[pixel] = 0.0.xx;
        return;
    }

    // NRD motion guide: engine velocity is NDC (curr - prev); NRD wants uvPrev - uvCurr.
    const float2 velocityNdc = g_VelocityMap.Load(int3(gbufferPixel, 0)).rg;
    g_MotionOutput[pixel] = float2(-0.5 * velocityNdc.x, 0.5 * velocityNdc.y);

    const float4 worldH = mul(g_InvViewProj, float4(clipXY, depth, 1.0));
    const float3 worldPos = worldH.xyz / worldH.w;
    const float3 rawNormal = g_NormalMap.Load(int3(gbufferPixel, 0)).xyz;
    const float rawNormalLength = length(rawNormal);
    const float roughness = g_Material0Map.Load(int3(gbufferPixel, 0)).a;
    const float3 shadingNormal = rawNormal / max(rawNormalLength, 1e-4);

    // Guides (encoding contract matches reflections.hlsl / NRD_NORMAL_ENCODING=3). Written for
    // every non-sky pixel, including the early-out cases below, so SIGMA always has valid guides.
    g_ViewZOutput[pixel] = mul(g_WorldToView, float4(worldPos, 1.0)).z;
    g_NormalRoughnessOutput[pixel] = float4(shadingNormal * 0.5 + 0.5, roughness);

    // Broken (MSAA-averaged) silhouette texel: the resolved normal is a blend of two surfaces and
    // the reconstructed position floats between them, so an occlusion trace here is unreliable.
    // Mark LIT (neutral), NOT shadowed — writing penumbra 0 paints a hard black outline around
    // every object edge (the aliased silhouette rim). SIGMA fills these thin edges from neighbors.
    if (rawNormalLength < 0.9)
    {
        g_PenumbraOutput[pixel] = NRD_FP16_MAX;
        return;
    }

    const float3 sunDir = normalize(g_SunDirection);
    const float ndotl = dot(shadingNormal, sunDir);

    // Back-facing the sun: no direct sun light reaches the surface (self-shadowed).
    // distanceToOccluder = 0 packs to penumbra 0 (hard shadow).
    if (ndotl <= 0.0)
    {
        g_PenumbraOutput[pixel] = SIGMA_FrontEnd_PackPenumbra(0.0, g_SunAngularTanRadius);
        return;
    }

    // Cone jitter for a soft penumbra: offset the direction inside the sun's angular cone.
    const float4 xi = RandomXi4(pixel, g_FrameIndex, 0u);
    float3 tangent;
    float3 bitangent;
    BuildTangentFrame(sunDir, tangent, bitangent);
    const float diskRadius = sqrt(xi.x);
    const float diskPhi = 2.0 * kPi * xi.y;
    const float2 disk = float2(diskRadius * cos(diskPhi), diskRadius * sin(diskPhi));
    const float3 rayDir = normalize(
        sunDir + (tangent * disk.x + bitangent * disk.y) * g_SunAngularTanRadius);

    // Modest shadow bias along the normal (distance-scaled), plus a mild grazing term to avoid
    // acne where the sun grazes the surface. Do NOT use the reflections' aggressive grazing
    // inflation — for shadows an over-large offset lifts the origin past nearby occluders and
    // leaks light ("islands" of no shadow inside shadowed regions).
    const float surfaceDistance = length(worldPos - g_CameraPos);
    const float grazing = saturate(1.0 - ndotl); // 0 head-on, ~1 near the terminator
    const float3 rayOrigin = worldPos
        + shadingNormal * (max(surfaceDistance * 0.0006, 0.005) * (1.0 + grazing));

    RayDesc ray;
    ray.Origin = rayOrigin;
    ray.Direction = rayDir;
    ray.TMin = 0.001;
    ray.TMax = max(g_MaxTraceDistance, 0.1);

    ShadowPayload payload;
    payload.hitT = NRD_FP16_MAX; // overwritten by the closest-hit shader on a blocker

    TraceRay(g_SceneTlas, kShadowRayFlags, 0xFF, 0, 0, 0, ray, payload);

    g_PenumbraOutput[pixel] = SIGMA_FrontEnd_PackPenumbra(payload.hitT, g_SunAngularTanRadius);
}

[shader("miss")]
void ShadowMiss(inout ShadowPayload payload)
{
    // No blocker along the shadow ray: keep the "lit" sentinel so the pack returns NRD_FP16_MAX.
    payload.hitT = NRD_FP16_MAX;
}

[shader("closesthit")]
void ShadowClosestHit(inout ShadowPayload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    // Occlusion only: report the blocker distance for SIGMA's penumbra pack. No shading.
    payload.hitT = RayTCurrent();
}
