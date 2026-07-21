#ifndef PBR_LIGHTING_CORE_HLSLI
#define PBR_LIGHTING_CORE_HLSLI

// Shared PBR lighting/shadow/IBL math for the raster paths. Included by BOTH the classic forward
// shader (pbr.ps.hlsl) and the mesh-shader G-buffer lighting (via mesh_lighting.hlsli), so the
// Cook-Torrance BRDF, cascaded PCF/PCSS shadow sampling, and SH diffuse irradiance live in one place.
//
// The functions reference the classic pbr.ps symbol names (uShadowMap, uCascadeEndSplits, uLightCount
// helpers, PI, LIGHT_TYPE_*, etc.). pbr.ps declares those natively; mesh_lighting.hlsli provides them
// via #define aliases onto its own 16-byte-aligned cbuffer + t7/t8/t9 resources. Each includer MUST
// declare all referenced cbuffer members, textures (uShadowMap/uPrefilterMap/uBrdfLut), samplers,
// PI, LIGHT_TYPE_* and include this file AFTER those declarations.

float3 SrgbToLinear(float3 srgb)
{
    return pow(srgb, 2.2.xxx);
}

float3 LinearToSrgb(float3 linearColor)
{
    return pow(max(linearColor, 0.0.xxx), 1.0.xxx / 2.2);
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
    // Engine lights store +Y "toward source" (see LightGizmoRenderer shineDirection = -towardSource).
    // glTF KHR_lights_punctual uses -Z emission; convert on import if/when glTF lights are loaded.
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

// Point-texel depth for manual compare tests (PCF averages compare results, not depth).
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

float FetchShadowDepthRaw(int cascadeIndex, float2 shadowUv)
{
    return FetchShadowDepth(cascadeIndex, shadowUv);
}

float3 DebugShadowBoundsColor(bool inBounds, float clipZ)
{
    if (inBounds)
    {
        return -1.0.xxx;
    }
    if (clipZ < 0.0)
    {
        return float3(0.15, 0.15, 0.85);
    }
    if (clipZ > 1.0)
    {
        return float3(0.85, 0.15, 0.15);
    }
    return float3(0.15, 0.85, 0.15);
}

float3 WorldToShadowSampleCoords(int cascadeIndex, float3 worldPos)
{
    float4 lightSpace = mul(uLightSpaceMatrices[cascadeIndex], float4(worldPos, 1.0));
    float3 coords = lightSpace.xyz / lightSpace.w;
    // LH ZO NDC -> shadow-map UV (top-left origin, Y flip for D3D12 depth sampling).
    coords.xy = coords.xy * 0.5 + 0.5;
    coords.y = 1.0 - coords.y;
    return coords;
}

float NormalizeCascadeClipDepth(int cascadeIndex, float stableClipZ)
{
    float depthMin = uCascadeClipDepthMin[cascadeIndex];
    float depthMax = uCascadeClipDepthMax[cascadeIndex];
    return saturate((stableClipZ - depthMin) / max(depthMax - depthMin, 1e-5));
}

float VisualizeClipDepth(int cascadeIndex, float clipZ)
{
    return NormalizeCascadeClipDepth(cascadeIndex, clipZ);
}

bool IsShadowBlocked(int cascadeIndex, float2 shadowUv, float compareDepth, float minSeparation)
{
    float storedDepth = FetchShadowDepth(cascadeIndex, shadowUv);
    return (compareDepth - storedDepth) > minSeparation;
}

// Mode 22 arbiter: raw center-texel compare with no receiver bias or min-separation floor.
bool IsShadowBlockedRawCenterTexel(int cascadeIndex, float3 worldPos)
{
    float3 sampleCoords = WorldToShadowSampleCoords(cascadeIndex, worldPos);
    if (sampleCoords.z < 0.0 || sampleCoords.z > 1.0
        || any(sampleCoords.xy < 0.0) || any(sampleCoords.xy > 1.0))
    {
        return false;
    }

    float storedDepth = FetchShadowDepth(cascadeIndex, sampleCoords.xy);
    return sampleCoords.z > storedDepth;
}

float ShadowFilterRotation(int cascadeIndex)
{
    return (float)cascadeIndex * 0.78539816339 + 0.39269908169;
}

static const int kMaxRotatedGridRadius = 4;

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
    int radius = min(kernelRadius, kMaxRotatedGridRadius);
    float phi = ShadowFilterRotation(cascadeIndex);
    float cosine = cos(phi);
    float sine = sin(phi);
    float2x2 rotation = float2x2(cosine, -sine, sine, cosine);
    float2 texelStep = texelSize * (filterRadiusTexels / max((float)radius, 1.0));
    float radiusSquared = max((float)(radius * radius), 1.0);

    float shadow = 0.0;
    float weightSum = 0.0;
    [loop]
    for (int x = -kMaxRotatedGridRadius; x <= kMaxRotatedGridRadius; ++x)
    {
        [loop]
        for (int y = -kMaxRotatedGridRadius; y <= kMaxRotatedGridRadius; ++y)
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
        int kernelRadius = min((int)ceil(filterRadius), kMaxRotatedGridRadius);
        return FilterShadowRotatedGrid(
            cascadeIndex, shadowUv, compareDepth, minSeparation, texelSize, filterRadius, kernelRadius);
    }

    int kernelRadius = (int)ceil(filterRadius);
    kernelRadius = min(kernelRadius, kMaxRotatedGridRadius);
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

float SampleCascadeShadow(int cascadeIndex, float3 worldPos, float3 geomNormal, float3 lightDir)
{
    return SampleCascadeShadowInternal(cascadeIndex, worldPos, geomNormal, lightDir, true);
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

float3 DebugDepthOutOfBoundsColor()
{
    return float3(1.0, 0.0, 1.0);
}

bool IsInShadowCascadeBounds(float3 sampleCoords)
{
    return sampleCoords.z >= 0.0 && sampleCoords.z <= 1.0
        && all(sampleCoords.xy >= 0.0) && all(sampleCoords.xy <= 1.0);
}

int SelectCascadeForReceiverWorldPos(float3 worldPos, float viewDepth)
{
    [loop]
    for (int candidateIndex = 0; candidateIndex < uCascadeCount; ++candidateIndex)
    {
        float3 sampleCoords = WorldToShadowSampleCoords(candidateIndex, worldPos);
        if (IsInShadowCascadeBounds(sampleCoords))
        {
            return candidateIndex;
        }
    }

    return clamp(SelectCascadeIndex(viewDepth), 0, max(uCascadeCount - 1, 0));
}

struct UnbiasedShadowSample
{
    int cascadeIndex;
    float2 shadowUv;
    float receiverClipZ;
    float compareDepth;
    float minSeparation;
    bool inBounds;
};

UnbiasedShadowSample BuildUnbiasedShadowSample(float3 worldPos, float viewDepth)
{
    UnbiasedShadowSample sampleData;
    sampleData.cascadeIndex = SelectCascadeIndex(viewDepth);
    float2 texelSize = 1.0 / float2(uShadowMapResolution, uShadowMapResolution);
    float texelUvSpan = max(texelSize.x, texelSize.y);
    sampleData.minSeparation = texelUvSpan * max(0.75, 1.25 * uDepthBiasScale);
    float3 sampleCoords = WorldToShadowSampleCoords(sampleData.cascadeIndex, worldPos);
    sampleData.receiverClipZ = sampleCoords.z;
    sampleData.shadowUv = sampleCoords.xy;
    sampleData.inBounds = sampleCoords.z >= 0.0 && sampleCoords.z <= 1.0
        && all(sampleCoords.xy >= 0.0) && all(sampleCoords.xy <= 1.0);
    float depthBias = texelUvSpan * 0.25 * uDepthBiasScale;
    sampleData.compareDepth = clamp(sampleCoords.z - depthBias, 0.0, 1.0);
    return sampleData;
}

float ComputeCascadeBlendFactor(float viewDepth)
{
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
            return smoothstep(blendStart, splitDistance, viewDepth);
        }
    }

    return 0.0;
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

#endif // PBR_LIGHTING_CORE_HLSLI
