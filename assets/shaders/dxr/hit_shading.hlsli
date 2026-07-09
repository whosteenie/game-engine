// DXR shared hit-shading core (Phase D9, devdoc/dxr/diffuse-gi.md).
// Single source for reflection and diffuse-GI closest-hit material evaluation: geometry
// accessors, RNG, environment sampling, and ShadeHit(). reflections.hlsl includes this
// header and adds reflection-specific trace logic (GGX, visibility, GI bounce).
//
// The cbuffer layout MUST stay byte-identical to DxrRootSignature::ReflectionDispatchConstants.

#ifndef DXR_HIT_SHADING_HLSLI
#define DXR_HIT_SHADING_HLSLI

#include "dxr_geometry_types.hlsli"

cbuffer ReflectionDispatchConstants : register(b0)
{
    uint2 g_OutputSize;
    uint2 g_GBufferSize;
    float4x4 g_InvViewProj;  // jittered, matches the depth buffer
    float4x4 g_ViewProj;     // jittered (unused by GI; kept for layout parity)
    float4x4 g_WorldToView;  // for linear viewZ (NRD guide)
    float3 g_CameraPos;
    float g_MaxTraceDistance;
    float g_EnvironmentIntensity;
    float g_MaxReflectionLod;
    uint g_FrameIndex;
    uint g_SamplesPerPixel;
    float3 g_SunDirection;
    float g_SunIntensity;
    float3 g_SunColor;
    uint g_AoRayCount; // reflection-only AO ray count; unused by the GI pass (kept for layout parity)
    float4 g_IrradianceSh9[9]; // L2 SH diffuse irradiance
    float g_RoughnessCutoff; // reflection-only; unused by GI (layout parity)
    float g_SunAngularTanRadius;
    float g_GiStrength;
    uint g_HasGiTrace;
    float4 _PadUnjitteredViewProj;
    float4x4 g_UnjitteredViewProj;
    float4x4 g_PrevViewProj;
    // F2 path-tracer-only: emissive NEE (zero for non-PT dispatches).
    uint g_EmissiveLightCount;
    float g_EmissiveLightPickWeightSum;
    float g_PtBloomHaloIntensity;
    float _PadPtEmissiveNee; // path-tracer-only: radiance isolate mode (see RenderDebug.h)
};

RaytracingAccelerationStructure g_SceneTlas : register(t0);
Texture2D<float> g_DepthMap : register(t1);
Texture2D<float4> g_NormalMap : register(t2);    // shading normal (RT2)
Texture2D<float4> g_Material0Map : register(t3); // albedo.rgb + roughness.a (RT5)

StructuredBuffer<GeometryLookupEntry> g_GeometryLookup : register(t4);
StructuredBuffer<float> g_SceneVertexFloats : register(t5);
StructuredBuffer<uint> g_SceneIndices : register(t6);

#include "dxr_geometry.hlsli"

TextureCube<float4> g_PrefilterMap : register(t10);
Texture2D<float4> g_VelocityMap : register(t11); // RT4 motion NDC (curr - prev)

// Per-object material constants (indexed by InstanceID). Layout mirrors DxrMaterialEntry.
struct MaterialEntry
{
    float3 albedo;
    float metallic;
    float3 emissive;
    float roughness;
    uint albedoTexIndex;        // absolute bindless SRV heap index; 0xFFFFFFFF = none
    uint albedoUvOffsetFloats;  // UV0 float offset within the vertex stride
    float transmission;         // 0 = opaque, 1 = glass (PT-A)
    float indexOfRefraction;    // dielectric IOR (air = 1.0); default ~1.5 glass
    float thinWalled;           // 1 = thin slab (pane); 0 = solid volume (lens)
    float _padDielectric;
};

StructuredBuffer<MaterialEntry> g_Materials : register(t12);

// Bindless: the whole shader-visible SRV heap, indexed by absolute descriptor index (space1).
Texture2D<float4> g_BindlessTextures[] : register(t0, space1);

SamplerState g_LinearClampSampler : register(s0);
SamplerState g_LinearWrapSampler : register(s1); // tiling albedo UVs

static const float kPi = 3.14159265;
static const float kMaxRadiance = 64.0;

// Integer PCG hash (pcg3d, Jarzynski & Olano). Frame-varying — a static per-pixel sequence
// makes the temporal denoiser converge TO the noise (RTQ-01).
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

// Decorrelated per pixel, per frame, per sample. [0; 1).
float2 RandomXi(uint2 pixel, uint frameIndex, uint sampleIndex)
{
    const uint3 hash = Pcg3d(uint3(pixel.x, pixel.y, frameIndex * 64u + sampleIndex));
    return float2(hash.xy & 0x00FFFFFFu) * (1.0 / 16777216.0);
}

// Four decorrelated randoms per sample: xy = lobe sample, zw = sub-pixel ray-setup jitter.
float4 RandomXi4(uint2 pixel, uint frameIndex, uint sampleIndex)
{
    const uint3 hashA = Pcg3d(uint3(pixel.x, pixel.y, frameIndex * 64u + sampleIndex));
    const uint3 hashB = Pcg3d(uint3(pixel.y ^ 0x9E3779B9u, pixel.x, frameIndex * 64u + sampleIndex + 32u));
    return float4(
        float2(hashA.xy & 0x00FFFFFFu) * (1.0 / 16777216.0),
        float2(hashB.xy & 0x00FFFFFFu) * (1.0 / 16777216.0));
}

float Luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

void BuildTangentFrame(float3 normal, out float3 tangent, out float3 bitangent)
{
    const float3 up = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    tangent = normalize(cross(up, normal));
    bitangent = cross(normal, tangent);
}

float3 ClampRadiance(float3 radiance)
{
    // Sanitize first: the HDR skybox sun exceeds fp16 (65504) and reads back as +Inf from the
    // RGBA16F prefilter. The luminance rescale below then computes Inf*(64/Inf) = NaN, which the
    // denoiser smears into a black splotch. Kill NaN (n != n) and clamp Inf/huge to finite.
    radiance.x = (radiance.x == radiance.x) ? radiance.x : 0.0;
    radiance.y = (radiance.y == radiance.y) ? radiance.y : 0.0;
    radiance.z = (radiance.z == radiance.z) ? radiance.z : 0.0;
    radiance = clamp(radiance, 0.0.xxx, 65504.0.xxx);
    const float luminance = dot(radiance, float3(0.2126, 0.7152, 0.0722));
    if (luminance <= kMaxRadiance)
    {
        return radiance;
    }

    return radiance * (kMaxRadiance / max(luminance, 1e-4));
}

float3 SampleEnvironment(float3 direction, float roughness)
{
    const float3 radiance = g_PrefilterMap.SampleLevel(
        g_LinearClampSampler,
        direction,
        roughness * g_MaxReflectionLod).rgb * g_EnvironmentIntensity;
    return min(radiance, 65504.0.xxx); // clamp fp16 Inf (huge HDR sun) to finite at the source
}

float3 LoadObjectNormal(GeometryLookupEntry geo, uint vertexIndex)
{
    const uint base = geo.vertexFloatOffset + vertexIndex * geo.vertexStrideFloats;
    return float3(
        g_SceneVertexFloats[base + 3],
        g_SceneVertexFloats[base + 4],
        g_SceneVertexFloats[base + 5]);
}

float2 LoadObjectUv(GeometryLookupEntry geo, uint vertexIndex, uint uvOffsetFloats)
{
    const uint base = geo.vertexFloatOffset + vertexIndex * geo.vertexStrideFloats + uvOffsetFloats;
    return float2(g_SceneVertexFloats[base + 0], g_SceneVertexFloats[base + 1]);
}

float2 ComputeHitUv(GeometryLookupEntry geo, uint primitiveIndex, uint uvOffsetFloats, float2 bary)
{
    const uint indexBase = geo.indexUintOffset + primitiveIndex * 3u;
    const uint i0 = g_SceneIndices[indexBase + 0];
    const uint i1 = g_SceneIndices[indexBase + 1];
    const uint i2 = g_SceneIndices[indexBase + 2];

    const float2 uv0 = LoadObjectUv(geo, i0, uvOffsetFloats);
    const float2 uv1 = LoadObjectUv(geo, i1, uvOffsetFloats);
    const float2 uv2 = LoadObjectUv(geo, i2, uvOffsetFloats);

    const float w = 1.0 - bary.x - bary.y;
    return uv0 * w + uv1 * bary.x + uv2 * bary.y;
}

float3 ComputeWorldShadingNormal(GeometryLookupEntry geo, uint primitiveIndex, float2 barycentrics)
{
    if (geo.vertexStrideFloats < 6u)
    {
        return ComputeWorldGeometricNormal(geo, primitiveIndex);
    }

    const uint indexBase = geo.indexUintOffset + primitiveIndex * 3u;
    const uint i0 = g_SceneIndices[indexBase + 0];
    const uint i1 = g_SceneIndices[indexBase + 1];
    const uint i2 = g_SceneIndices[indexBase + 2];

    const float3 n0 = LoadObjectNormal(geo, i0);
    const float3 n1 = LoadObjectNormal(geo, i1);
    const float3 n2 = LoadObjectNormal(geo, i2);

    const float w = 1.0 - barycentrics.x - barycentrics.y;
    const float3 objectNormal = n0 * w + n1 * barycentrics.x + n2 * barycentrics.y;
    if (dot(objectNormal, objectNormal) < 1e-8)
    {
        return ComputeWorldGeometricNormal(geo, primitiveIndex);
    }

    const float3x4 objectToWorld = ObjectToWorld3x4();
    const float3 worldNormal = mul((float3x3)objectToWorld, objectNormal);
    return normalize(worldNormal);
}

float3 SrgbToLinear(float3 c)
{
    return pow(max(c, 0.0.xxx), 2.2.xxx);
}

// L2 SH diffuse irradiance evaluation. Matches EvaluateDiffuseIrradianceSh in pbr.ps.hlsl.
float3 EvaluateDiffuseIrradianceSh(float3 normal)
{
    const float3 n = normalize(normal);
    const float x = n.x;
    const float y = n.y;
    const float z = n.z;

    float3 irradiance = g_IrradianceSh9[0].rgb * 0.282095;
    irradiance += g_IrradianceSh9[1].rgb * (0.488603 * y);
    irradiance += g_IrradianceSh9[2].rgb * (0.488603 * z);
    irradiance += g_IrradianceSh9[3].rgb * (0.488603 * x);
    irradiance += g_IrradianceSh9[4].rgb * (1.092548 * x * y);
    irradiance += g_IrradianceSh9[5].rgb * (1.092548 * y * z);
    irradiance += g_IrradianceSh9[6].rgb * (0.315392 * (3.0 * z * z - 1.0));
    irradiance += g_IrradianceSh9[7].rgb * (1.092548 * z * x);
    irradiance += g_IrradianceSh9[8].rgb * (0.546274 * (x * x - y * y));
    return max(irradiance, 0.0.xxx);
}

// Fresnel-Schlick with a roughness-aware ceiling (matches FresnelSchlickRoughness in pbr.ps.hlsl).
float3 FresnelSchlickRoughnessGi(float cosTheta, float3 f0, float roughness)
{
    const float3 maxReflection = max(1.0.xxx - roughness, f0);
    return f0 + (maxReflection - f0) * pow(saturate(1.0 - cosTheta), 5.0);
}

float3 FresnelSchlickRoughnessReflection(float cosTheta, float3 f0, float roughness)
{
    return FresnelSchlickRoughnessGi(cosTheta, f0, roughness);
}

// Cosine-weighted hemisphere direction about a normal (Malley's method) for ambient occlusion.
float3 CosineSampleHemisphere(float3 normal, float2 xi)
{
    float3 tangent;
    float3 bitangent;
    BuildTangentFrame(normal, tangent, bitangent);
    const float radius = sqrt(saturate(xi.x));
    const float phi = 2.0 * kPi * xi.y;
    const float z = sqrt(max(1.0 - xi.x, 0.0));
    return normalize(tangent * (radius * cos(phi)) + bitangent * (radius * sin(phi)) + normal * z);
}

// Analytic split-sum environment BRDF (Karis, "Physically Based Shading on Mobile"). Same term the
// reflection trace uses (reflections.hlsl EnvBrdfApprox) so the GI bounce's specular matches it.
float3 EnvBrdfApprox(float3 f0, float roughness, float nDotV)
{
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    const float4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28 * nDotV)) * r.x + r.y;
    const float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

// Full one-bounce outgoing radiance at a ray hit, toward the receiver: the surface's diffuse
// response PLUS its specular lobe (environment reflected + split-sum BRDF) PLUS emissive. The
// specular term matters for glossy/metal hits: their outgoing radiance toward the receiver is
// dominated by the reflected environment (a mirror bounces the sky/scene onward), not diffuse — a
// diffuse-only bounce returned ~0 for them, both losing that transport and creating a high-variance
// zero-vs-sky split the diffuse denoiser smears into splotches. Env is the terminal bounce (no
// secondary trace), matching reflections.hlsl's ReflectionClosestHit.
// viewDir = direction from the hit toward the receiver (i.e. -incoming ray direction).
float3 ShadeHit(
    uint instanceId, uint primitiveIndex, float2 barycentrics, float3 hitNormal, float3 viewDir)
{
    const GeometryLookupEntry geo = g_GeometryLookup[instanceId];
    const MaterialEntry material = g_Materials[instanceId];

    float3 albedo = material.albedo;
    if (material.albedoTexIndex != 0xFFFFFFFFu && material.albedoUvOffsetFloats != 0xFFFFFFFFu)
    {
        const float2 hitUv =
            ComputeHitUv(geo, primitiveIndex, material.albedoUvOffsetFloats, barycentrics);
        const float3 texel =
            g_BindlessTextures[NonUniformResourceIndex(material.albedoTexIndex)]
                .SampleLevel(g_LinearWrapSampler, hitUv, 0.0).rgb;
        albedo *= texel;
    }

    // Energy-conserving diffuse weight, matching the raster (pbr.ps.hlsl CalcCookTorrance): the
    // fraction of light NOT taken by the specular lobe. Without the (1 - F) term the bounce
    // surface reads hotter than it does under direct shading, inflating GI.
    const float3 f0 = lerp(0.04.xxx, albedo, material.metallic);
    const float nDotV = saturate(dot(hitNormal, viewDir));
    const float3 specularEnergy = FresnelSchlickRoughnessGi(nDotV, f0, max(material.roughness, 0.55));
    const float3 diffuseEnergy = (1.0.xxx - specularEnergy) * (1.0 - material.metallic);
    const float3 diffuseAlbedo = albedo * diffuseEnergy;

    const float3 sunL = normalize(g_SunDirection);
    const float ndotl = saturate(dot(hitNormal, sunL));
    const float3 sunRadiance = SrgbToLinear(g_SunColor) * g_SunIntensity;
    const float3 direct = diffuseAlbedo * sunRadiance * ndotl / kPi;

    const float3 irradiance = EvaluateDiffuseIrradianceSh(hitNormal);
    const float3 ambient = diffuseAlbedo * irradiance / kPi;

    // Specular environment IBL at the hit: reflect the incoming ray about the hit normal, sample the
    // prefiltered env at the surface roughness, and weight by the split-sum env BRDF. This is the
    // reflected-light transport the surface bounces toward the receiver (a mirror floor sends the sky
    // up onto the receiver). f0 = 0.04 for dielectrics, albedo for metals, so metals return their
    // tinted reflection while the (1 - F) diffuse split above keeps energy conserved.
    const float3 reflectDir = reflect(-viewDir, hitNormal);
    const float3 specular =
        SampleEnvironment(reflectDir, material.roughness) * EnvBrdfApprox(f0, material.roughness, nDotV);

    return max(direct + ambient + specular + material.emissive, 0.0.xxx);
}

#endif // DXR_HIT_SHADING_HLSLI
