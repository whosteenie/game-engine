// DXR Phase D4/D5 — specular reflection trace + NRD guide buffers
// (see devdoc/dxr-reflections.md and devdoc/dxr-nrd-integration.md).
// u0: RGBA16F radiance (rgb) + HIT DISTANCE (a) — RELAX_FrontEnd_PackRadianceAndHitDist
//     convention (raw world units; miss = maxTraceDistance).
// u1: R32F linear viewZ (sky = large value beyond the denoising range)
// u2: RGBA8 normal+roughness — MUST match the NRD compile options set in CMakeLists
//     (NRD_NORMAL_ENCODING=0: rgb = N*0.5+0.5; NRD_ROUGHNESS_ENCODING=1: a = linear roughness)
// u3: RG16F screen-space motion, NRD convention mv = uvPrev - uvCurr (motionVectorScale {1,1})
// Hit shading v2: in-hit analytic material shading — albedo x (sun NdotL + SH9 ambient) +
// emissive, using a per-object material table (t12) and smooth vertex normals. Diffuse only
// (no reflected sun shadow). Miss: prefiltered environment.

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
    float _padSun;
    float4 g_IrradianceSh9[9]; // L2 SH diffuse irradiance
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

    const float3 viewVec = normalize(worldPos - g_CameraPos); // camera -> surface
    const float3 mirrorDir = reflect(viewVec, shadingNormal);

    const float surfaceDistance = length(worldPos - g_CameraPos);
    const float3 rayOrigin =
        worldPos + shadingNormal * max(surfaceDistance * 0.0015, 0.01);

    const uint sampleCount = clamp(g_SamplesPerPixel, 1u, 16u);
    float3 radianceSum = 0.0.xxx;
    float hitDistSum = 0.0;
    float radianceWeightSum = 0.0;
    float sampleCountF = 0.0;

    [loop]
    for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
    {
        // IMPORTANT: the sequence MUST vary per frame. NRD's temporal accumulation converges
        // frame-varying noise toward the true mean; a static per-pixel sequence latches pixels
        // onto whatever their fixed directions hit (bright emissives!) every single frame, so
        // the denoiser converges TO the speckle instead of averaging it away.
        const float2 xi = RandomXi(pixel, g_FrameIndex, sampleIndex);

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

        // Karis firefly-weighted accumulation: a plain arithmetic mean lets a single very
        // bright sample (emissive hit) dominate the pixel forever — one 50-luminance hit in
        // 16 samples leaves a mean of ~3 on a 0.05-luminance surface, i.e. a permanent dot.
        // Weighting each sample by 1/(1+luma) forms a robust mean with negligible bias.
        const float3 sampleRadiance = ClampRadiance(payload.radiance);
        const float sampleWeight = 1.0 / (1.0 + Luminance(sampleRadiance));
        radianceSum += sampleRadiance * sampleWeight;
        radianceWeightSum += sampleWeight;
        hitDistSum += payload.hitDistance;
        sampleCountF += 1.0;
    }

    // RELAX packing: radiance + raw hit distance in world units (miss = maxTraceDistance).
    g_ReflectionOutput[pixel] = float4(
        radianceSum / max(radianceWeightSum, 1e-4),
        hitDistSum / max(sampleCountF, 1.0));
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

    const float3 sunL = normalize(g_SunDirection);
    const float ndotl = saturate(dot(hitNormal, sunL));
    const float3 sunRadiance = SrgbToLinear(g_SunColor) * g_SunIntensity;
    const float3 direct = diffuseAlbedo * sunRadiance * ndotl / kPi;

    const float3 irradiance = EvaluateDiffuseIrradianceSh(hitNormal);
    const float3 ambient = diffuseAlbedo * irradiance / kPi;

    payload.radiance = max(direct + ambient + material.emissive, 0.0.xxx);

    const float distance01 = saturate(hitT / max(g_MaxTraceDistance, 1e-4));
    const float distanceWeight = 1.0 - distance01 * distance01;
    const float facingWeight = saturate(dot(hitNormal, -rayDir));
    payload.confidence = distanceWeight * facingWeight;
}
