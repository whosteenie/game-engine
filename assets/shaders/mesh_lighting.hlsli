#ifndef MESH_LIGHTING_HLSLI
#define MESH_LIGHTING_HLSLI

// Self-contained forward lighting for the mesh-shader G-buffer path.
//
// The math here is a faithful port of assets/shaders/pbr.ps.hlsl (the classic lit path). It is
// duplicated rather than shared to keep the working classic path completely untouched; the register
// bindings and constant-buffer layout differ (the mesh path feeds a clean 16-byte-aligned cbuffer
// so the C++ mirror is unambiguous). The `#define` aliases below let the ported functions read
// verbatim against the pbr.ps source so transcription drift is easy to audit. If pbr.ps lighting
// changes, mirror the change here. See devdoc/rendering/mesh-instancing-and-mesh-shaders.md.

#define MESH_MAX_LIGHTS 8
#define MESH_MAX_CASCADES 4

static const float MESH_PI = 3.14159265359;
static const int MESH_LIGHT_TYPE_DIRECTIONAL = 0;
static const int MESH_LIGHT_TYPE_POINT = 1;
static const int MESH_LIGHT_TYPE_SPOT = 2;

struct MeshLightData
{
    int4 typeAndFlags;  // x = light type
    float4 position;    // xyz = world position
    float4 direction;   // xyz = normalized direction toward the light source
    float4 color;       // rgb = sRGB light color
    float4 params0;     // x = intensity, y = attenConstant, z = attenLinear, w = attenQuadratic
    float4 params1;     // x = range, y = innerCutoffCos, z = outerCutoffCos
};

cbuffer MeshLightingConstants : register(b1)
{
    float4 uMeshViewPos;                                     // xyz = camera world position
    int4 uMeshLightMeta;                                     // x=lightCount y=shadowLightIndex z=receiveShadow w=shadowFilterMode
    MeshLightData uMeshLights[MESH_MAX_LIGHTS];
    float4x4 uMeshLightSpaceMatrices[MESH_MAX_CASCADES];
    float4 uMeshCascadeEndSplits;
    float4 uMeshCascadeTexelWorldSizes;
    float4 uMeshCascadeClipDepthMin;
    float4 uMeshCascadeClipDepthMax;
    float4 uMeshShadowParams0;  // x=cascadeBlendRatio y=cascadeCount z=cascadeNearPlane w=shadowMapResolution
    float4 uMeshShadowParams1;  // x=pcfKernelRadius y=usePoissonPcf z=minPenumbraTexels w=pcssBlockerRadius
    float4 uMeshShadowParams2;  // x=pcssLightAngularSize y=pcssMinPenumbraTexels z=pcssMaxPenumbraTexels w=worldBiasScale
    float4 uMeshShadowParams3;  // x=depthBiasScale
    float4 uMeshIblParams;      // x=maxReflectionLod y=environmentIntensity z=omitSpecularIbl w=omitDiffuseIbl
    float4 uMeshIrradianceSh[9];
};

Texture2DArray uMeshShadowMap : register(t7);
TextureCube uMeshPrefilterMap : register(t8);
Texture2D uMeshBrdfLut : register(t9);
SamplerState uMeshIblSampler : register(s1);

// Aliases: let the ported pbr.ps functions read identically against the shared source.
#define PI MESH_PI
#define LIGHT_TYPE_DIRECTIONAL MESH_LIGHT_TYPE_DIRECTIONAL
#define LIGHT_TYPE_POINT MESH_LIGHT_TYPE_POINT
#define LIGHT_TYPE_SPOT MESH_LIGHT_TYPE_SPOT
#define uShadowMap uMeshShadowMap
#define uPrefilterMap uMeshPrefilterMap
#define uBrdfLut uMeshBrdfLut
#define uPrefilterSampler uMeshIblSampler
#define uBrdfLutSampler uMeshIblSampler
#define uLightSpaceMatrices uMeshLightSpaceMatrices
#define uCascadeEndSplits uMeshCascadeEndSplits
#define uCascadeTexelWorldSizes uMeshCascadeTexelWorldSizes
#define uCascadeClipDepthMin uMeshCascadeClipDepthMin
#define uCascadeClipDepthMax uMeshCascadeClipDepthMax
#define uIrradianceSh uMeshIrradianceSh
#define uLightCount (uMeshLightMeta.x)
#define uShadowLightIndex (uMeshLightMeta.y)
#define uReceiveShadow (uMeshLightMeta.z)
#define uShadowFilterMode ((int)uMeshLightMeta.w)
#define uCascadeBlendRatio (uMeshShadowParams0.x)
#define uCascadeCount ((int)uMeshShadowParams0.y)
#define uCascadeNearPlane (uMeshShadowParams0.z)
#define uShadowMapResolution (uMeshShadowParams0.w)
#define uPcfKernelRadius ((int)uMeshShadowParams1.x)
#define uUsePoissonPcf ((int)uMeshShadowParams1.y)
#define uMinPenumbraTexels (uMeshShadowParams1.z)
#define uPcssBlockerRadius ((int)uMeshShadowParams1.w)
#define uPcssLightAngularSize (uMeshShadowParams2.x)
#define uPcssMinPenumbraTexels (uMeshShadowParams2.y)
#define uPcssMaxPenumbraTexels (uMeshShadowParams2.z)
#define uWorldBiasScale (uMeshShadowParams2.w)
#define uDepthBiasScale (uMeshShadowParams3.x)
#define uMaxReflectionLod (uMeshIblParams.x)
#define uEnvironmentIntensity (uMeshIblParams.y)
#define uOmitSpecularIbl ((int)uMeshIblParams.z)
#define uOmitDiffuseIbl ((int)uMeshIblParams.w)

float3 MeshSrgbToLinear(float3 srgb)
{
    return pow(srgb, 2.2.xxx);
}

float3 EvaluateDiffuseIrradianceSh(float3 normal)
{
    float3 n = normalize(normal);
    float x = n.x;
    float y = n.y;
    float z = n.z;

    float3 irradiance = uIrradianceSh[0].rgb * 0.282095;
    irradiance += uIrradianceSh[1].rgb * (0.488603 * y);
    irradiance += uIrradianceSh[2].rgb * (0.488603 * z);
    irradiance += uIrradianceSh[3].rgb * (0.488603 * x);
    irradiance += uIrradianceSh[4].rgb * (1.092548 * x * y);
    irradiance += uIrradianceSh[5].rgb * (1.092548 * y * z);
    irradiance += uIrradianceSh[6].rgb * (0.315392 * (3.0 * z * z - 1.0));
    irradiance += uIrradianceSh[7].rgb * (1.092548 * z * x);
    irradiance += uIrradianceSh[8].rgb * (0.546274 * (x * x - y * y));
    return max(irradiance, 0.0.xxx);
}

float DistributionGGX(float3 normal, float3 halfDir, float roughness)
{
    float alpha = roughness * roughness;
    float alphaSquared = alpha * alpha;
    float normalDotHalf = max(dot(normal, halfDir), 0.0);
    float normalDotHalfSquared = normalDotHalf * normalDotHalf;

    float numerator = alphaSquared;
    float denominator = normalDotHalfSquared * (alphaSquared - 1.0) + 1.0;
    denominator = PI * denominator * denominator;

    return numerator / max(denominator, 0.0001);
}

float GeometrySchlickGGX(float normalDotView, float roughness)
{
    float remappedRoughness = roughness + 1.0;
    float k = (remappedRoughness * remappedRoughness) / 8.0;

    float numerator = normalDotView;
    float denominator = normalDotView * (1.0 - k) + k;

    return numerator / max(denominator, 0.0001);
}

float GeometrySmith(float3 normal, float3 viewDir, float3 lightDir, float roughness)
{
    float normalDotView = max(dot(normal, viewDir), 0.0);
    float normalDotLight = max(dot(normal, lightDir), 0.0);
    float geometryView = GeometrySchlickGGX(normalDotView, roughness);
    float geometryLight = GeometrySchlickGGX(normalDotLight, roughness);
    return geometryView * geometryLight;
}

float3 FresnelSchlick(float cosineTheta, float3 f0)
{
    return f0 + (1.0 - f0) * pow(saturate(1.0 - cosineTheta), 5.0);
}

float3 FresnelSchlickRoughness(float cosineTheta, float3 f0, float roughness)
{
    float3 maxReflection = max(1.0.xxx - roughness, f0);
    return f0 + (maxReflection - f0) * pow(saturate(1.0 - cosineTheta), 5.0);
}

float CalcAttenuation(
    float constant,
    float linearAttenuation,
    float quadratic,
    float lightRange,
    float distance)
{
    float attenuation = 1.0 / (constant + linearAttenuation * distance + quadratic * distance * distance);

    if (lightRange > 0.0)
    {
        float rangeFactor = saturate(1.0 - pow(distance / lightRange, 4.0));
        attenuation *= rangeFactor * rangeFactor;
    }

    return attenuation;
}

float CalcSpotIntensity(
    float3 lightDir,
    float3 towardLightSource,
    float innerCutoffCos,
    float outerCutoffCos)
{
    float theta = dot(lightDir, normalize(towardLightSource));
    return saturate((theta - outerCutoffCos) / (innerCutoffCos - outerCutoffCos + 0.0001));
}

void CalcLightDirectionAndAttenuation(
    int lightType,
    float3 lightPosition,
    float3 lightDirection,
    float constant,
    float linearAttenuation,
    float quadratic,
    float lightRange,
    float innerCutoffCos,
    float outerCutoffCos,
    float3 fragPos,
    out float3 lightDir,
    out float attenuation,
    out float spotIntensity)
{
    if (lightType == LIGHT_TYPE_DIRECTIONAL)
    {
        lightDir = normalize(lightDirection);
        attenuation = 1.0;
        spotIntensity = 1.0;
        return;
    }

    float3 toLight = lightPosition - fragPos;
    float distance = length(toLight);
    lightDir = toLight / max(distance, 0.0001);
    attenuation = CalcAttenuation(constant, linearAttenuation, quadratic, lightRange, distance);
    spotIntensity = 1.0;

    if (lightType == LIGHT_TYPE_SPOT)
    {
        spotIntensity = CalcSpotIntensity(lightDir, lightDirection, innerCutoffCos, outerCutoffCos);
    }
}

float3 CalcCookTorranceContribution(
    float3 normal,
    float3 viewDir,
    float3 lightDir,
    float3 albedo,
    float roughness,
    float metallic,
    float3 radiance,
    float diffuseNdotL,
    float specularNdotL)
{
    float3 halfDir = normalize(viewDir + lightDir);

    float3 f0 = lerp(0.04.xxx, albedo, metallic);

    float normalDistribution = DistributionGGX(normal, halfDir, roughness);
    float geometry = GeometrySmith(normal, viewDir, lightDir, roughness);
    float3 fresnel = FresnelSchlick(max(dot(halfDir, viewDir), 0.0), f0);

    float3 numerator = normalDistribution * geometry * fresnel;
    float denominator = 4.0 * max(dot(normal, viewDir), 0.0) * max(specularNdotL, 0.0) + 0.0001;
    float3 specular = numerator / denominator;

    float3 specularEnergy = fresnel;
    float3 diffuseEnergy = (1.0.xxx - specularEnergy) * (1.0 - metallic);

    return diffuseEnergy * albedo / PI * radiance * diffuseNdotL
        + specular * radiance * specularNdotL;
}

float FetchShadowDepth(int cascadeIndex, float2 shadowUv)
{
    uint width = 0;
    uint height = 0;
    uint layers = 0;
    uShadowMap.GetDimensions(width, height, layers);
    const uint resolution = max(width, 1u);
    uint2 texel = uint2(shadowUv * float(resolution));
    texel = min(texel, uint2(resolution - 1, resolution - 1));
    return uShadowMap.Load(int4(texel.x, texel.y, cascadeIndex, 0)).r;
}

float3 WorldToShadowSampleCoords(int cascadeIndex, float3 worldPos)
{
    float4 lightSpace = mul(uLightSpaceMatrices[cascadeIndex], float4(worldPos, 1.0));
    float3 coords = lightSpace.xyz / lightSpace.w;
    coords.xy = coords.xy * 0.5 + 0.5;
    coords.y = 1.0 - coords.y;
    return coords;
}

bool IsShadowBlocked(int cascadeIndex, float2 shadowUv, float compareDepth, float minSeparation)
{
    float storedDepth = FetchShadowDepth(cascadeIndex, shadowUv);
    return (compareDepth - storedDepth) > minSeparation;
}

float ShadowFilterRotation(int cascadeIndex)
{
    return (float)cascadeIndex * 0.78539816339 + 0.39269908169;
}

static const int kMeshMaxRotatedGridRadius = 4;

float EffectiveMinPenumbraTexels()
{
    return clamp(uMinPenumbraTexels, 0.0, 8.0);
}

float ClampFilterRadiusTexels(float filterRadiusTexels)
{
    return clamp(filterRadiusTexels, 0.5, 12.0);
}

float FilterShadowRotatedGrid(
    int cascadeIndex,
    float2 shadowUv,
    float compareDepth,
    float minSeparation,
    float2 texelSize,
    float filterRadiusTexels,
    int kernelRadius)
{
    int radius = min(kernelRadius, kMeshMaxRotatedGridRadius);
    float phi = ShadowFilterRotation(cascadeIndex);
    float cosine = cos(phi);
    float sine = sin(phi);
    float2x2 rotation = float2x2(cosine, -sine, sine, cosine);
    float2 texelStep = texelSize * (filterRadiusTexels / max((float)radius, 1.0));
    float radiusSquared = max((float)(radius * radius), 1.0);

    float shadow = 0.0;
    float weightSum = 0.0;
    [loop]
    for (int x = -kMeshMaxRotatedGridRadius; x <= kMeshMaxRotatedGridRadius; ++x)
    {
        [loop]
        for (int y = -kMeshMaxRotatedGridRadius; y <= kMeshMaxRotatedGridRadius; ++y)
        {
            if (abs(x) > radius || abs(y) > radius)
            {
                continue;
            }

            float distSquared = (float)(x * x + y * y);
            float weight = exp(-distSquared / (radiusSquared * 0.45));
            float2 offset = mul(rotation, float2((float)x, (float)y)) * texelStep;
            shadow += (IsShadowBlocked(cascadeIndex, shadowUv + offset, compareDepth, minSeparation) ? 0.0 : 1.0) * weight;
            weightSum += weight;
        }
    }

    return shadow / max(weightSum, 1e-5);
}

float FilterShadowGridPcf(int cascadeIndex, float2 shadowUv, float compareDepth, float minSeparation, float2 texelSize, int kernelRadius)
{
    float shadow = 0.0;
    float sampleCount = 0.0;
    [loop]
    for (int x = -kernelRadius; x <= kernelRadius; ++x)
    {
        [loop]
        for (int y = -kernelRadius; y <= kernelRadius; ++y)
        {
            float2 offset = float2((float)x, (float)y) * texelSize;
            shadow += IsShadowBlocked(cascadeIndex, shadowUv + offset, compareDepth, minSeparation) ? 0.0 : 1.0;
            sampleCount += 1.0;
        }
    }

    return shadow / max(sampleCount, 1.0);
}

float FilterShadowPcf(int cascadeIndex, float2 shadowUv, float compareDepth, float minSeparation, float2 texelSize, int kernelRadius)
{
    if (uUsePoissonPcf != 0)
    {
        float filterRadiusTexels = ClampFilterRadiusTexels(
            max((float)kernelRadius, EffectiveMinPenumbraTexels()));
        return FilterShadowRotatedGrid(
            cascadeIndex, shadowUv, compareDepth, minSeparation, texelSize, filterRadiusTexels, kernelRadius);
    }

    return FilterShadowGridPcf(cascadeIndex, shadowUv, compareDepth, minSeparation, texelSize, kernelRadius);
}

float FilterShadowPcss(int cascadeIndex, float2 shadowUv, float compareDepth, float minSeparation, float2 texelSize)
{
    float blockerDepthSum = 0.0;
    int blockerCount = 0;

    [loop]
    for (int x = -uPcssBlockerRadius; x <= uPcssBlockerRadius; ++x)
    {
        [loop]
        for (int y = -uPcssBlockerRadius; y <= uPcssBlockerRadius; ++y)
        {
            float2 offset = float2((float)x, (float)y) * texelSize;
            float blockerDepth = FetchShadowDepth(cascadeIndex, shadowUv + offset);
            if ((compareDepth - blockerDepth) > minSeparation)
            {
                blockerDepthSum += blockerDepth;
                blockerCount++;
            }
        }
    }

    if (blockerCount == 0)
    {
        return FilterShadowPcf(cascadeIndex, shadowUv, compareDepth, minSeparation, texelSize, uPcfKernelRadius);
    }

    float avgBlockerDepth = blockerDepthSum / (float)blockerCount;
    float penumbraRatio = (compareDepth - avgBlockerDepth) / max(avgBlockerDepth, 1e-5);
    float filterRadius = penumbraRatio * uPcssLightAngularSize;
    filterRadius = clamp(
        filterRadius,
        max(uPcssMinPenumbraTexels, EffectiveMinPenumbraTexels()),
        uPcssMaxPenumbraTexels);
    filterRadius = ClampFilterRadiusTexels(filterRadius);
    filterRadius = max(EffectiveMinPenumbraTexels(), ceil(filterRadius));

    if (uUsePoissonPcf != 0)
    {
        int kernelRadius = min((int)ceil(filterRadius), kMeshMaxRotatedGridRadius);
        return FilterShadowRotatedGrid(
            cascadeIndex, shadowUv, compareDepth, minSeparation, texelSize, filterRadius, kernelRadius);
    }

    int kernelRadius = (int)ceil(filterRadius);
    kernelRadius = min(kernelRadius, kMeshMaxRotatedGridRadius);
    return FilterShadowGridPcf(cascadeIndex, shadowUv, compareDepth, minSeparation, texelSize, kernelRadius);
}

float SampleCascadeShadowInternal(
    int cascadeIndex,
    float3 worldPos,
    float3 geomNormal,
    float3 lightDir,
    bool applyReceiverBias)
{
    float3 normal = normalize(geomNormal);
    float nDotL = saturate(dot(normal, lightDir));

    float2 texelSize = 1.0 / float2(uShadowMapResolution, uShadowMapResolution);
    float texelUvSpan = max(texelSize.x, texelSize.y);
    float texelWorldSpan = uCascadeTexelWorldSizes[cascadeIndex];
    float minSeparation = texelUvSpan * max(0.75, 1.25 * uDepthBiasScale);

    float3 biasedWorldPos = worldPos;
    float depthBias = texelUvSpan * 0.25 * uDepthBiasScale;

    if (applyReceiverBias)
    {
        float sinTheta = sqrt(max(1.0 - nDotL * nDotL, 1e-5));
        float worldBias = texelWorldSpan * (1.5 + 3.5 * sinTheta / max(nDotL, 0.15)) * uWorldBiasScale;
        depthBias = texelUvSpan * (1.0 + 2.0 * sinTheta / max(nDotL, 0.15)) * uDepthBiasScale;
        float facingLight = saturate(nDotL / 0.15);
        biasedWorldPos = worldPos
            + normal * worldBias
            + lightDir * (texelWorldSpan * 2.0 * uWorldBiasScale * facingLight);
    }

    float3 sampleCoords = WorldToShadowSampleCoords(cascadeIndex, biasedWorldPos);

    if (sampleCoords.z < 0.0 || sampleCoords.z > 1.0)
    {
        return 1.0;
    }

    if (any(sampleCoords.xy < 0.0) || any(sampleCoords.xy > 1.0))
    {
        return 1.0;
    }

    float2 shadowUv = sampleCoords.xy;
    float compareDepth = clamp(sampleCoords.z - depthBias, 0.0, 1.0);

    return uShadowFilterMode == 1
        ? FilterShadowPcss(cascadeIndex, shadowUv, compareDepth, minSeparation, texelSize)
        : FilterShadowPcf(cascadeIndex, shadowUv, compareDepth, minSeparation, texelSize, uPcfKernelRadius);
}

int SelectCascadeIndex(float viewDepth)
{
    int cascadeIndex = 0;
    [loop]
    for (int i = 0; i < uCascadeCount - 1; ++i)
    {
        if (viewDepth > uCascadeEndSplits[i])
        {
            cascadeIndex = i + 1;
        }
    }
    return cascadeIndex;
}

float CalcShadowInternal(
    float3 fragPos,
    float viewDepth,
    float3 geomNormal,
    float3 lightDir,
    bool applyReceiverBias)
{
    if (uReceiveShadow == 0)
    {
        return 1.0;
    }

    [loop]
    for (int boundaryIndex = 0; boundaryIndex < uCascadeCount - 1; ++boundaryIndex)
    {
        float splitDistance = uCascadeEndSplits[boundaryIndex];
        float previousSplit = uCascadeNearPlane;
        if (boundaryIndex > 0)
        {
            previousSplit = uCascadeEndSplits[boundaryIndex - 1];
        }
        float cascadeSpan = max(splitDistance - previousSplit, 1e-4);
        float blendWidth = cascadeSpan * uCascadeBlendRatio;
        float blendStart = splitDistance - blendWidth;

        if (viewDepth > blendStart && viewDepth <= splitDistance)
        {
            float nearShadow = SampleCascadeShadowInternal(
                boundaryIndex, fragPos, geomNormal, lightDir, applyReceiverBias);
            float farShadow = SampleCascadeShadowInternal(
                boundaryIndex + 1, fragPos, geomNormal, lightDir, applyReceiverBias);
            float blendFactor = smoothstep(blendStart, splitDistance, viewDepth);
            return lerp(nearShadow, farShadow, blendFactor);
        }
    }

    int cascadeIndex = SelectCascadeIndex(viewDepth);
    return SampleCascadeShadowInternal(cascadeIndex, fragPos, geomNormal, lightDir, applyReceiverBias);
}

float CalcShadow(float3 fragPos, float viewDepth, float3 geomNormal, float3 lightDir)
{
    return CalcShadowInternal(fragPos, viewDepth, geomNormal, lightDir, true);
}

// Surface inputs the mesh-shader PS resolves from the instance/material table + interpolants.
struct MeshLightingSurface
{
    float3 worldPos;
    float3 shadingNormal;  // normal-mapped
    float3 geomNormal;     // interpolated geometric normal (shadow visibility gate)
    float3 albedo;         // linear
    float roughness;       // authored (0..1)
    float metallic;
    float3 emissive;       // linear
    float viewDepth;       // view-space z (cascade selection)
};

// Split-lighting outputs matching the classic pbr.ps split-lighting contract consumed by the
// radiance-assembly pass: oDirect = fill lights + emissive, oIndirect = ambient (IBL),
// oSunShadow = (unshadowed sun radiance, shadow factor).
struct MeshLightingResult
{
    float3 direct;
    float3 indirect;
    float3 sunUnshadowed;
    float shadowFactor;
};

MeshLightingResult ComputeMeshSplitLighting(MeshLightingSurface surface)
{
    float3 normal = normalize(surface.shadingNormal);
    float3 geomNormalNorm = normalize(surface.geomNormal);
    float3 viewDir = normalize(uMeshViewPos.xyz - surface.worldPos);

    float3 albedo = surface.albedo;
    float roughness = clamp(surface.roughness, 0.0, 1.0);
    float metallic = clamp(surface.metallic, 0.0, 1.0);
    const float roughnessBrdf = max(roughness, 0.04);
    float3 emissiveLinear = max(surface.emissive, 0.0.xxx);

    float3 f0 = lerp(0.04.xxx, albedo, metallic);
    const float ambientOcclusion = 1.0;

    float3 irradiance = EvaluateDiffuseIrradianceSh(normal);
    float3 diffuseIbl = irradiance * albedo / PI;

    float3 reflection = reflect(-viewDir, normal);
    float3 prefilteredColor = uPrefilterMap.SampleLevel(uPrefilterSampler, reflection, roughness * uMaxReflectionLod).rgb;
    float2 envBrdf = uBrdfLut.Sample(uBrdfLutSampler, float2(max(dot(normal, viewDir), 0.0), roughness)).rg;
    float3 specularIbl = prefilteredColor * (f0 * envBrdf.x + envBrdf.y);

    float nDotVGeom = max(dot(geomNormalNorm, viewDir), 0.0);
    float roughnessForIndirectEnergy = max(roughness, 0.55);
    float3 specularEnergy = FresnelSchlickRoughness(nDotVGeom, f0, roughnessForIndirectEnergy);
    float3 diffuseEnergy = (1.0.xxx - specularEnergy) * (1.0 - metallic);
    const float3 indirectSpecular = uOmitSpecularIbl != 0 ? 0.0.xxx : specularIbl;
    const float3 indirectDiffuse = uOmitDiffuseIbl != 0 ? 0.0.xxx : diffuseEnergy * diffuseIbl;
    float3 ambient = (indirectDiffuse + indirectSpecular) * uEnvironmentIntensity * ambientOcclusion;

    float3 directFillUnshadowed = 0.0.xxx;
    float3 directSunUnshadowed = 0.0.xxx;
    float shadowFactor = 1.0;

    [loop]
    for (int i = 0; i < uLightCount; ++i)
    {
        MeshLightData light = uMeshLights[i];
        int lightType = light.typeAndFlags.x;

        float3 lightDir;
        float attenuation;
        float spotIntensity;
        CalcLightDirectionAndAttenuation(
            lightType,
            light.position.xyz,
            light.direction.xyz,
            light.params0.y,
            light.params0.z,
            light.params0.w,
            light.params1.x,
            light.params1.y,
            light.params1.z,
            surface.worldPos,
            lightDir,
            attenuation,
            spotIntensity);

        float3 radiance = MeshSrgbToLinear(light.color.rgb) * light.params0.x;
        float perturbedNdotL = max(dot(normal, lightDir), 0.0);
        float geomNdotL = max(dot(geomNormalNorm, lightDir), 0.0);
        float diffuseNdotL = perturbedNdotL;
        if (i == uShadowLightIndex && lightType == LIGHT_TYPE_DIRECTIONAL)
        {
            diffuseNdotL = geomNdotL > 0.0 ? perturbedNdotL : 0.0;
        }

        float3 contribution = CalcCookTorranceContribution(
            normal,
            viewDir,
            lightDir,
            albedo,
            roughnessBrdf,
            metallic,
            radiance,
            diffuseNdotL,
            perturbedNdotL);

        float3 litContribution = contribution * attenuation * spotIntensity;

        if (i == uShadowLightIndex)
        {
            shadowFactor = CalcShadow(surface.worldPos, surface.viewDepth, surface.geomNormal, lightDir);
            directSunUnshadowed += litContribution;
        }
        else
        {
            directFillUnshadowed += litContribution;
        }
    }

    if (uShadowLightIndex < 0)
    {
        shadowFactor = 1.0;
    }

    MeshLightingResult result;
    result.direct = directFillUnshadowed + emissiveLinear;
    result.indirect = ambient;
    result.sunUnshadowed = directSunUnshadowed;
    result.shadowFactor = shadowFactor;
    return result;
}

#endif // MESH_LIGHTING_HLSLI
