// DXR Phase D4/D5 — specular reflection trace + NRD guide buffers
// (see devdoc/dxr-reflections.md and devdoc/dxr-nrd-integration.md).
// u0: RGBA16F radiance (rgb) + HIT DISTANCE (a) — RELAX_FrontEnd_PackRadianceAndHitDist
//     convention (raw world units; miss = maxTraceDistance).
// u1: R32F linear viewZ (sky = large value beyond the denoising range)
// u2: RGBA8 normal+roughness — MUST match the NRD compile options set in CMakeLists
//     (NRD_NORMAL_ENCODING=0: rgb = N*0.5+0.5; NRD_ROUGHNESS_ENCODING=1: a = linear roughness)
// u3: RG16F screen-space motion, NRD convention mv = uvPrev - uvCurr (motionVectorScale {1,1})
// Hit shading v3: in-hit analytic material shading — albedo x (sun NdotL + SH9 ambient) +
// specular env IBL + emissive, using a per-object material table (t12) and smooth vertex
// normals. Diffuse direct is gated by a traced sun-visibility ray and the SH ambient by traced
// ambient occlusion (needs MaxTraceRecursionDepth >= 2) so reflected surfaces get the same
// contact darkening the primary view gets from screen-space AO/shadows. Miss: prefiltered env.

cbuffer ReflectionDispatchConstants : register(b0)
{
    uint2 g_OutputSize;
    uint2 g_GBufferSize;
    float4x4 g_InvViewProj;  // jittered, matches the depth buffer
    float4x4 g_ViewProj;     // jittered, for hit -> screen reprojection
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
    uint g_AoRayCount; // reflected-hit ambient-occlusion rays (0 = AO off), tunable
    float4 g_IrradianceSh9[9]; // L2 SH diffuse irradiance
    float g_RoughnessCutoff; // receivers rougher than this skip the trace (env fallback), tunable
    float3 _padCutoff;
};

RWTexture2D<float4> g_ReflectionOutput : register(u0);
RWTexture2D<float> g_ViewZOutput : register(u1);
RWTexture2D<float4> g_NormalRoughnessOutput : register(u2);
RWTexture2D<float2> g_MotionOutput : register(u3);

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
    uint _pad0;
    uint _pad1;
};

StructuredBuffer<MaterialEntry> g_Materials : register(t12);

// Bindless: the whole shader-visible SRV heap, indexed by absolute descriptor index. Used to
// sample per-object albedo textures at the hit point (space1 avoids clashing with t0..t12).
Texture2D<float4> g_BindlessTextures[] : register(t0, space1);

SamplerState g_LinearClampSampler : register(s0);
SamplerState g_LinearWrapSampler : register(s1); // tiling albedo UVs

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

// Integer PCG hash (pcg3d, Jarzynski & Olano). The previous frac()-based float hash lost
// precision as frameIndex grew and was strongly correlated: many pixels received a nearly
// STATIC sample sequence, permanently latching onto bright hits — the temporal denoiser
// then converged to a stable speckle pattern instead of averaging the noise away.
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

// Four decorrelated randoms per sample: xy = GGX lobe, zw = sub-pixel ray-setup jitter.
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

// GGX VNDF half-vector sampling (Heitz 2018, "Sampling the GGX Distribution of Visible
// Normals"). Generates only half-vectors VISIBLE from the view direction — unlike plain NDF
// sampling this (a) almost never produces below-surface reflection directions (our previous
// mirror-direction fallback for those was a bias/variance source right at grazing edges) and
// (b) has substantially lower variance at grazing incidence, where the shimmer lived.
// viewWorld: direction from surface toward the camera (unit). Returns a world-space half-vector.
float3 SampleGgxVndfHalfVector(float3 normal, float3 viewWorld, float roughness, float2 xi)
{
    const float alpha = max(roughness * roughness, 1e-3);

    float3 tangent;
    float3 bitangent;
    BuildTangentFrame(normal, tangent, bitangent);

    // View direction in tangent space (z along the normal).
    const float3 viewTangent = float3(
        dot(viewWorld, tangent),
        dot(viewWorld, bitangent),
        dot(viewWorld, normal));

    // Stretch view by alpha (transforms GGX to the hemisphere configuration).
    const float3 stretchedView =
        normalize(float3(alpha * viewTangent.x, alpha * viewTangent.y, viewTangent.z));

    // Orthonormal basis around the stretched view.
    const float lenSq = stretchedView.x * stretchedView.x + stretchedView.y * stretchedView.y;
    const float3 basis1 = lenSq > 1e-7
        ? float3(-stretchedView.y, stretchedView.x, 0.0) * rsqrt(lenSq)
        : float3(1.0, 0.0, 0.0);
    const float3 basis2 = cross(stretchedView, basis1);

    // Uniform disk sample warped onto the visible hemisphere.
    const float r = sqrt(xi.x);
    const float phi = 2.0 * kPi * xi.y;
    const float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    const float s = 0.5 * (1.0 + stretchedView.z);
    t2 = (1.0 - s) * sqrt(saturate(1.0 - t1 * t1)) + s * t2;

    const float3 halfStretched = t1 * basis1 + t2 * basis2
        + sqrt(saturate(1.0 - t1 * t1 - t2 * t2)) * stretchedView;

    // Unstretch back to the GGX configuration.
    const float3 halfTangent = normalize(float3(
        alpha * halfStretched.x,
        alpha * halfStretched.y,
        max(halfStretched.z, 1e-6)));

    return normalize(
        tangent * halfTangent.x + bitangent * halfTangent.y + normal * halfTangent.z);
}

float3 ClampRadiance(float3 radiance)
{
    // Sanitize first: the HDR skybox sun exceeds fp16 (65504) and reads back as +Inf from the
    // RGBA16F prefilter. The luminance rescale below then computes Inf*(64/Inf) = NaN, which NRD's
    // a-trous filter smears into a black splotch on mirror reflections. Kill NaN (n != n) and
    // clamp Inf/huge to a finite ceiling before the rescale.
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
    // Clamp the fp16 Inf (a > 65504 HDR sun) to a finite value at the source so the sky/MSAA
    // fallback writes (which skip ClampRadiance) also stay finite.
    return min(radiance, 65504.0.xxx);
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

float3 LoadObjectNormal(GeometryLookupEntry geo, uint vertexIndex)
{
    // Vertex layout is interleaved position(3) + normal(3) + ...; normal sits at offset 3.
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

// Barycentric-interpolated UV0 for the hit triangle.
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

// Barycentric-interpolated smooth world normal. Falls back to the geometric face normal when
// the mesh has no per-vertex normals (stride < 6).
float3 ComputeWorldShadingNormal(
    GeometryLookupEntry geo, uint primitiveIndex, float2 barycentrics)
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

    // Transform the normal as a direction; uniform/rigid transforms dominate scene objects, so
    // ObjectToWorld's rotation is a good approximation without the inverse-transpose.
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

// Analytic split-sum environment BRDF (Karis, "Physically Based Shading on Mobile"). Avoids
// binding a BRDF LUT in the reflection root signature; accurate enough for the secondary-bounce
// specular term at a reflection hit.
float3 EnvBrdfApprox(float3 f0, float roughness, float nDotV)
{
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    const float4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28 * nDotV)) * r.x + r.y;
    const float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
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

// Binary visibility along a ray. The probe SKIPS the closest-hit shader and ends at the first
// hit, so it never re-enters ReflectionClosestHit (recursion stays <= 2, matching the RTPSO's
// MaxTraceRecursionDepth) and costs only a traversal. Returns 1.0 when the ray reaches the miss
// shader (unoccluded), 0.0 when anything is hit. Requires MaxTraceRecursionDepth >= 2.
float TraceVisibility(float3 origin, float3 direction, float tMax)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.001;
    ray.TMax = tMax;

    Payload probe;
    probe.radiance = 0.0.xxx;
    probe.confidence = 0.0;
    probe.hitDistance = 0.0;
    probe.surfaceRoughness = 0.0;
    probe.hit = 1; // assume occluded; ReflectionMiss clears this to 0 when the ray escapes
    probe._pad = 0;

    const uint occlusionFlags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
        | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_FORCE_OPAQUE;
    TraceRay(g_SceneTlas, occlusionFlags, 0xFF, 0, 0, 0, ray, probe);
    return probe.hit == 0 ? 1.0 : 0.0;
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

    // Sky: env in the raw view; viewZ pushed beyond the denoising range so NRD ignores it.
    const float2 clipXY = DepthUvToClipXY(uv);
    if (depth >= 0.9999)
    {
        const float4 farH = mul(g_InvViewProj, float4(clipXY, 1.0, 1.0));
        const float3 viewDir = normalize(farH.xyz / farH.w - g_CameraPos);
        g_ReflectionOutput[pixel] = float4(SampleEnvironment(viewDir, 0.0), g_MaxTraceDistance);
        g_ViewZOutput[pixel] = 1e6;
        g_NormalRoughnessOutput[pixel] = float4(0.5, 0.5, 1.0, 1.0);
        // Sky has no surface motion — zero MV avoids spurious history reprojection streaks.
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

    // MSAA-resolved silhouette rejection (RTQ-01): at partially covered pixels the resolved
    // G-buffer averages TWO surfaces — the depth reconstructs a position floating in space
    // and the normal is a blend of disagreeing directions (its length shrinks below 1).
    // Rays launched from such pixels hit random geometry (bright emissives especially) and
    // the bias is CONSISTENT, so temporal accumulation converges to speckle instead of
    // averaging it away. Hand these pixels to the IBL fallback (hitDist = miss).
    const float3 shadingNormal = rawNormal / max(rawNormalLength, 1e-4);
    if (rawNormalLength < 0.9)
    {
        const float3 mirrorFallback = reflect(normalize(worldPos - g_CameraPos), shadingNormal);
        g_ReflectionOutput[pixel] =
            float4(SampleEnvironment(mirrorFallback, roughness), g_MaxTraceDistance);
        g_ViewZOutput[pixel] = mul(g_WorldToView, float4(worldPos, 1.0)).z;
        g_NormalRoughnessOutput[pixel] = float4(shadingNormal * 0.5 + 0.5, roughness);
        return;
    }

    // NRD guides (encoding contract documented at the top of this file).
    g_ViewZOutput[pixel] = mul(g_WorldToView, float4(worldPos, 1.0)).z;
    g_NormalRoughnessOutput[pixel] = float4(shadingNormal * 0.5 + 0.5, roughness);

    const float3 viewVecCenter = normalize(worldPos - g_CameraPos); // camera -> surface (center)

    // Roughness cutoff (perf + quality): surfaces rougher than the cutoff produce a diffuse-wide,
    // noisy scatter that the denoiser just smears into a blurry blob — and diffuse GI/IBL already
    // covers that indirect. Skip the per-pixel TraceRay entirely and write the prefiltered-env
    // fallback along the mirror direction. The composite fades RT->IBL over the same cutoff, so
    // the handoff is seamless. Guides above were already written for RELAX.
    if (roughness >= g_RoughnessCutoff)
    {
        const float3 mirrorDir = reflect(viewVecCenter, shadingNormal);
        g_ReflectionOutput[pixel] =
            float4(SampleEnvironment(mirrorDir, roughness), g_MaxTraceDistance);
        return;
    }

    const uint sampleCount = clamp(g_SamplesPerPixel, 1u, 16u);
    float3 radianceSum = 0.0.xxx;
    float radianceWeightSum = 0.0;
    // Representative hit distance for the NRD guide: the CLOSEST actual hit. Averaging hits
    // with maxTrace misses fed NRD a value that wobbled frame-to-frame at edge pixels, and
    // RELAX drives specular reprojection/filter footprints from it — direct shimmer source.
    float closestHitDist = 1e30;
    bool anyHit = false;

    [loop]
    for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
    {
        // IMPORTANT: the sequence MUST vary per frame. NRD's temporal accumulation converges
        // frame-varying noise toward the true mean; a static per-pixel sequence latches pixels
        // onto whatever their fixed directions hit (bright emissives!) every single frame, so
        // the denoiser converges TO the speckle instead of averaging it away.
        const float4 xi = RandomXi4(pixel, g_FrameIndex, sampleIndex);

        // Reflection anti-aliasing: jitter the RAY SETUP sub-pixel per sample (xi.zw).
        // Without this, mirror surfaces (roughness <= 0.03) fire N identical rays — zero
        // anti-aliasing of the reflected image no matter the sample count. Jittered setup
        // integrates both the reflected image and the receiver's own edge footprint.
        const float2 sampleUv = (float2(pixel) + xi.zw) / float2(g_OutputSize);
        const int2 sampleGbufferPixel = int2(sampleUv * float2(g_GBufferSize));
        const float sampleDepth = g_DepthMap.Load(int3(sampleGbufferPixel, 0)).r;
        const float3 sampleRawNormal = g_NormalMap.Load(int3(sampleGbufferPixel, 0)).xyz;
        const float sampleNormalLength = length(sampleRawNormal);
        const float sampleRoughness = g_Material0Map.Load(int3(sampleGbufferPixel, 0)).a;

        float3 sampleRadiance;

        if (sampleDepth >= 0.9999 || sampleNormalLength < 0.9)
        {
            // Sub-sample landed on sky or a broken (MSAA-averaged) texel: environment along
            // the center mirror direction; counts as a miss for the composite.
            sampleRadiance = ClampRadiance(
                SampleEnvironment(reflect(viewVecCenter, shadingNormal), roughness));
        }
        else
        {
            const float2 sampleClipXY = DepthUvToClipXY(sampleUv);
            const float4 sampleWorldH = mul(g_InvViewProj, float4(sampleClipXY, sampleDepth, 1.0));
            const float3 sampleWorldPos = sampleWorldH.xyz / sampleWorldH.w;
            const float3 sampleNormal = sampleRawNormal / sampleNormalLength;
            const float3 sampleViewVec = normalize(sampleWorldPos - g_CameraPos);
            const float3 sampleMirror = reflect(sampleViewVec, sampleNormal);

            float3 rayDir = sampleMirror;
            if (sampleRoughness > 0.03)
            {
                const float3 halfVector = SampleGgxVndfHalfVector(
                    sampleNormal, -sampleViewVec, sampleRoughness, xi.xy);
                const float3 sampled = reflect(sampleViewVec, halfVector);
                // VNDF makes below-surface directions vanishingly rare; keep the mirror
                // fallback purely as numerical insurance.
                if (dot(sampled, sampleNormal) > 1e-4)
                {
                    rayDir = normalize(sampled);
                }
            }

            // Grazing-aware origin bias: at glancing incidence rays creep along the surface
            // and intermittently self-hit (edge shimmer) — push the origin out further.
            const float sampleDistance = length(sampleWorldPos - g_CameraPos);
            const float nDotV = saturate(dot(sampleNormal, -sampleViewVec));
            const float3 rayOrigin = sampleWorldPos
                + sampleNormal * (max(sampleDistance * 0.0015, 0.01) * (1.0 + 2.0 * (1.0 - nDotV)));

            RayDesc ray;
            ray.Origin = rayOrigin;
            ray.Direction = rayDir;
            ray.TMin = 0.001;
            ray.TMax = max(g_MaxTraceDistance, 0.1);

            Payload payload;
            payload.radiance = 0.0.xxx;
            payload.confidence = 0.0;
            payload.hitDistance = 0.0;
            payload.surfaceRoughness = sampleRoughness;
            payload.hit = 0;
            payload._pad = 0;

            TraceRay(g_SceneTlas, kReflectionRayFlags, 0xFF, 0, 0, 0, ray, payload);

            sampleRadiance = ClampRadiance(payload.radiance);
            if (payload.hit != 0)
            {
                closestHitDist = min(closestHitDist, payload.hitDistance);
                anyHit = true;
            }
        }

        // Karis firefly-weighted accumulation: a plain arithmetic mean lets a single very
        // bright sample (emissive hit) dominate the pixel forever — one 50-luminance hit in
        // 16 samples leaves a mean of ~3 on a 0.05-luminance surface, i.e. a permanent dot.
        // Weighting each sample by 1/(1+luma) forms a robust mean with negligible bias.
        const float sampleWeight = 1.0 / (1.0 + Luminance(sampleRadiance));
        radianceSum += sampleRadiance * sampleWeight;
        radianceWeightSum += sampleWeight;
    }

    // RELAX packing: radiance + raw hit distance in world units (miss = maxTraceDistance).
    g_ReflectionOutput[pixel] = float4(
        radianceSum / max(radianceWeightSum, 1e-4),
        anyHit ? closestHitDist : g_MaxTraceDistance);
}

[shader("miss")]
void ReflectionMiss(inout Payload payload)
{
    // Ray left the scene: prefiltered environment IS the correct radiance. Hit distance is
    // reported as the trace range ("hit at infinity") — NRD uses it for reprojection and the
    // D6 composite will treat far hits as IBL-equivalent.
    payload.radiance = SampleEnvironment(WorldRayDirection(), payload.surfaceRoughness);
    payload.confidence = 0.0;
    payload.hitDistance = g_MaxTraceDistance;
    payload.hit = 0;
}

[shader("closesthit")]
void ReflectionClosestHit(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    const uint instanceId = InstanceID();
    const uint primitiveIndex = PrimitiveIndex();
    const GeometryLookupEntry geo = g_GeometryLookup[instanceId];

    const float3 rayDir = WorldRayDirection();
    const float hitT = RayTCurrent();

    float3 hitNormal = ComputeWorldShadingNormal(geo, primitiveIndex, attribs.barycentrics);
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

    // In-hit analytic material shading (devdoc/dxr-nrd-integration.md). Evaluate the surface's
    // own diffuse response instead of reusing screen-space radiance — this is what makes rough
    // objects reflect their own color rather than surrounding geometry, and stays correct at
    // grazing / off-screen angles. v1: diffuse only, no reflected sun shadow.
    const MaterialEntry material = g_Materials[instanceId];

    // Textured surfaces carry their color in the albedo map, not the constant. Sample it bindless
    // at the interpolated hit UV (SRGB texture -> linear on sample). UNORM albedo maps modulate the
    // constant, which stays white for pure-textured materials.
    float3 albedo = material.albedo;
    if (material.albedoTexIndex != 0xFFFFFFFFu && material.albedoUvOffsetFloats != 0xFFFFFFFFu)
    {
        const float2 hitUv =
            ComputeHitUv(geo, primitiveIndex, material.albedoUvOffsetFloats, attribs.barycentrics);
        const float3 texel =
            g_BindlessTextures[NonUniformResourceIndex(material.albedoTexIndex)]
                .SampleLevel(g_LinearWrapSampler, hitUv, 0.0).rgb;
        albedo *= texel;
    }
    const float3 diffuseAlbedo = albedo * (1.0 - material.metallic);

    // Occlusion at the hit. The primary view darkens contact crevices via screen-space AO and
    // cascade shadows; neither is available for an off-screen reflection hit, so without tracing
    // it here the reflected copy of an object stays fully lit where its primary copy is dark —
    // the object-colored halo seen where geometry clips into a mirror. Origin is pushed off the
    // surface along the geometric-ish shading normal to avoid self-intersection.
    const float3 hitPos = WorldRayOrigin() + rayDir * hitT;
    const float3 occlusionOrigin = hitPos + hitNormal * max(hitT * 0.001, 0.002);
    const uint2 dispatchPixel = DispatchRaysIndex().xy;

    // Occlusion only modulates the DIFFUSE terms, so it's invisible on surfaces with no diffuse
    // lobe. Skip every occlusion ray on metals and near-black materials — a large saving on
    // metallic reflections at zero visual cost (perf: dxr-groundwork.md).
    const float diffuseWeight = max(diffuseAlbedo.r, max(diffuseAlbedo.g, diffuseAlbedo.b));
    const bool needsOcclusion = diffuseWeight > 0.02;

    const float3 sunL = normalize(g_SunDirection);
    const float ndotl = saturate(dot(hitNormal, sunL));
    const float3 sunRadiance = SrgbToLinear(g_SunColor) * g_SunIntensity;
    // Only pay for the shadow ray where the sun actually contributes (front-facing + visible diffuse).
    const float sunVisibility = (needsOcclusion && ndotl > 0.0)
        ? TraceVisibility(occlusionOrigin, sunL, g_MaxTraceDistance)
        : 1.0;
    const float3 direct = diffuseAlbedo * sunRadiance * ndotl / kPi * sunVisibility;

    // Cosine-weighted ambient occlusion over a bounded radius (contact-scale). Ray count is a
    // quality/perf tunable (g_AoRayCount): more rays = less noise before the RELAX specular
    // denoiser smooths it. Skipped entirely on metals/dark surfaces (needsOcclusion) or when the
    // count is 0.
    const float aoRadius = max(g_MaxTraceDistance * 0.05, 0.5);
    const uint aoRayCount = g_AoRayCount;
    const bool traceAo = needsOcclusion && aoRayCount > 0u;
    float aoVisibility = 1.0;
    if (traceAo)
    {
        float aoSum = 0.0;
        [loop]
        for (uint aoIndex = 0u; aoIndex < aoRayCount; ++aoIndex)
        {
            const float2 aoXi = RandomXi(dispatchPixel, g_FrameIndex, aoIndex + 8u);
            const float3 aoDir = CosineSampleHemisphere(hitNormal, aoXi);
            aoSum += TraceVisibility(occlusionOrigin, aoDir, aoRadius);
        }
        aoVisibility = aoSum / float(aoRayCount);
    }

    const float3 irradiance = EvaluateDiffuseIrradianceSh(hitNormal);
    const float3 ambient = diffuseAlbedo * irradiance / kPi * aoVisibility;

    // Specular environment IBL at the hit (multi-bounce v1). Reflect the incoming ray about the hit
    // normal, sample the prefiltered environment at the surface's roughness, and weight it by
    // Fresnel + analytic env BRDF. This is what stops a metal/mirror SEEN IN a reflection from
    // reading black: metals have no diffuse lobe, so without this term their reflected image is just
    // emissive. A true "hall of mirrors" would trace a secondary ray here; this env sample is the
    // terminal approximation for that bounce.
    const float3 viewVec = -rayDir;
    const float nDotV = saturate(dot(hitNormal, viewVec));
    const float3 f0 = lerp(0.04.xxx, albedo, material.metallic);
    const float3 reflectDir = reflect(rayDir, hitNormal);
    const float3 specular =
        SampleEnvironment(reflectDir, material.roughness) * EnvBrdfApprox(f0, material.roughness, nDotV);

    payload.radiance = max(direct + ambient + specular + material.emissive, 0.0.xxx);

    const float distance01 = saturate(hitT / max(g_MaxTraceDistance, 1e-4));
    const float distanceWeight = 1.0 - distance01 * distance01;
    const float facingWeight = saturate(dot(hitNormal, -rayDir));
    payload.confidence = distanceWeight * facingWeight;
}
