#define MAX_LIGHTS 8
#define MAX_CASCADES 4

static const float PI = 3.14159265359;

static const int LIGHT_TYPE_DIRECTIONAL = 0;
static const int LIGHT_TYPE_POINT = 1;
static const int LIGHT_TYPE_SPOT = 2;

TextureCube uPrefilterMap : register(t2);
Texture2D uBrdfLut : register(t3);
Texture2D uAlbedoMap : register(t4);
Texture2D uNormalMap : register(t5);
Texture2D uAoMap : register(t6);
Texture2D uRoughnessMap : register(t7);
Texture2DArray uShadowMap : register(t8);
Texture2D uEmissiveMap : register(t9);

SamplerState uPrefilterSampler : register(s2);
SamplerState uBrdfLutSampler : register(s3);
SamplerState uAlbedoSampler : register(s4);
SamplerState uNormalSampler : register(s5);
SamplerState uAoSampler : register(s6);
SamplerState uRoughnessSampler : register(s7);
SamplerState uShadowMapSampler : register(s8);

cbuffer PerPixel : register(b0)
{
    float3 uViewPos;
    float _pad0;
    float3 uAlbedo;
    float uRoughness;
    float uMetallic;
    float3 uEmissive;
    int uUseAlbedoMap;
    int uUseNormalMap;
    int uUseAoMap;
    int uUseRoughnessMap;
    int uUseMetallicRoughnessMap;
    int uUseEmissiveMap;
    int uAlbedoTexCoordSet;
    int uNormalTexCoordSet;
    int uAoTexCoordSet;
    int uRoughnessTexCoordSet;
    int uEmissiveTexCoordSet;
    int uLightCount;
    int uLightTypes[MAX_LIGHTS];
    float4 uLightPositions[MAX_LIGHTS];
    float4 uLightDirections[MAX_LIGHTS];
    float4 uLightColors[MAX_LIGHTS];
    float uLightIntensities[MAX_LIGHTS];
    float uLightAttenConstant[MAX_LIGHTS];
    float uLightAttenLinear[MAX_LIGHTS];
    float uLightAttenQuadratic[MAX_LIGHTS];
    float uLightRange[MAX_LIGHTS];
    float uLightInnerCutoffCos[MAX_LIGHTS];
    float uLightOuterCutoffCos[MAX_LIGHTS];
    float4x4 uLightSpaceMatrices[MAX_CASCADES];
    float uCascadeEndSplits[MAX_CASCADES];
    float uCascadeTexelWorldSizes[MAX_CASCADES];
    float uCascadeClipDepthMin[MAX_CASCADES];
    float uCascadeClipDepthMax[MAX_CASCADES];
    float uCascadeStableOrthoNear[MAX_CASCADES];
    float uCascadeStableOrthoFar[MAX_CASCADES];
    float uCascadeContentOrthoNear[MAX_CASCADES];
    float uCascadeContentOrthoFar[MAX_CASCADES];
    float uCascadeBlendRatio;
    int uCascadeCount;
    float uCascadeNearPlane;
    int uShadowFilterMode;
    int uPcfKernelRadius;
    int uUsePoissonPcf;
    float uMinPenumbraTexels;
    int uPcssBlockerRadius;
    float uPcssLightAngularSize;
    float uPcssMinPenumbraTexels;
    float uPcssMaxPenumbraTexels;
    float uWorldBiasScale;
    float uDepthBiasScale;
    float uShadowMapResolution;
    float4x4 uLightSpaceMatrix;
    float4x4 uView;
    int uShadowLightIndex;
    int uReceiveShadow;
    int uOutputLinear;
    int uSplitLightingOutput;
    int uDebugMode;
    float uMaxReflectionLod;
    float uEnvironmentIntensity;
    // When set, omit specular IBL from the indirect (RT1) output — a reflection composite
    // (RT reflections or SSR) will ADD it back at full precision. Avoids baking the fp16-Inf
    // HDR sun into RT1 only to subtract it back out (which sparkled at reflected highlights).
    int uOmitSpecularIbl;
    // When set, omit the SH diffuse ambient from the indirect (RT1) output — RT diffuse GI will
    // REPLACE it (the cosine-hemisphere trace already integrates the sky + one bounce + true
    // occlusion, so it supersedes the crude 9-coefficient SH sky). Avoids double-counting.
    int uOmitDiffuseIbl;
    float4 uIrradianceSh[9];
};

struct PSInput
{
    float4 position : SV_Position;
    float3 fragPos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 texCoord0 : TEXCOORD2;
    float2 texCoord1 : TEXCOORD3;
    float4 tangent : TEXCOORD4;
    float4 fragPosLightSpace : TEXCOORD5;
    float viewDepth : TEXCOORD6;
    float4 currClip : TEXCOORD7;
    float4 prevClip : TEXCOORD8;
};

struct PSOutput
{
    float4 oDirect : SV_Target0;
    float4 oIndirect : SV_Target1;
    float4 oNormal : SV_Target2;
    float4 oSunShadow : SV_Target3;
    float4 oVelocity : SV_Target4;
    float4 oMaterial0 : SV_Target5;
    float4 oMaterial1 : SV_Target6;
};

void WriteGBufferMaterials(
    inout PSOutput output,
    float3 albedoLinear,
    float roughness,
    float metallic,
    float3 emissiveLinear)
{
    output.oMaterial0 = float4(albedoLinear, roughness);
    output.oMaterial1 = float4(metallic, emissiveLinear);
}

float2 ComputeMotionNdc(float4 currClip, float4 prevClip)
{
    float2 currNdc = currClip.xy / currClip.w;
    float2 prevNdc = prevClip.xy / prevClip.w;
    return currNdc - prevNdc;
}

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

float2 SelectTexCoord(int texCoordSet, float2 texCoord0, float2 texCoord1)
{
    return texCoordSet == 1 ? texCoord1 : texCoord0;
}

float3 CalcNormalFromMap(float3 normal, float4 tangent, float2 texCoord)
{
    float3 tangentNormal = uNormalMap.Sample(uNormalSampler, texCoord).rgb * 2.0 - 1.0;

    float3 tangentVector = normalize(tangent.xyz);
    tangentVector = normalize(tangentVector - dot(tangentVector, normal) * normal);
    float3 bitangent = normalize(cross(normal, tangentVector) * tangent.w);
    float3x3 tbn = float3x3(tangentVector, bitangent, normal);

    return normalize(mul(tangentNormal, tbn));
}

float3 ToViewNormal(float3 worldNormal)
{
    return normalize(mul((float3x3)uView, worldNormal));
}

PSOutput main(PSInput input)
{
    PSOutput output;

    float3 normal = normalize(input.normal);
    float3 geomNormal = normal;
    float2 albedoTexCoord = SelectTexCoord(uAlbedoTexCoordSet, input.texCoord0, input.texCoord1);
    float2 normalTexCoord = SelectTexCoord(uNormalTexCoordSet, input.texCoord0, input.texCoord1);
    float2 aoTexCoord = SelectTexCoord(uAoTexCoordSet, input.texCoord0, input.texCoord1);
    float2 roughnessTexCoord = SelectTexCoord(uRoughnessTexCoordSet, input.texCoord0, input.texCoord1);
    float2 emissiveTexCoord = SelectTexCoord(uEmissiveTexCoordSet, input.texCoord0, input.texCoord1);
    if (uUseNormalMap != 0)
    {
        normal = CalcNormalFromMap(normal, input.tangent, normalTexCoord);
    }

    float3 geomNormalNorm = normalize(geomNormal);
    float3 viewDir = normalize(uViewPos - input.fragPos);

    float3 albedo = uAlbedo;
    if (uUseAlbedoMap != 0)
    {
        // uAlbedo is linear (glTF baseColorFactor / inspector converts sRGB picks). Albedo map is UNORM_SRGB.
        float3 albedoSample = uAlbedoMap.Sample(uAlbedoSampler, albedoTexCoord).rgb;
        albedo *= albedoSample;
    }

    float roughness = uRoughness;
    float metallic = uMetallic;
    if (uUseMetallicRoughnessMap != 0)
    {
        float3 metallicRoughnessSample = uRoughnessMap.Sample(uRoughnessSampler, roughnessTexCoord).rgb;
        roughness *= metallicRoughnessSample.g;
        metallic *= metallicRoughnessSample.b;
    }
    else if (uUseRoughnessMap != 0)
    {
        roughness *= uRoughnessMap.Sample(uRoughnessSampler, roughnessTexCoord).r;
    }
    roughness = clamp(roughness, 0.04, 1.0);
    metallic = clamp(metallic, 0.0, 1.0);

    float3 emissiveLinear = max(uEmissive, 0.0.xxx);
    if (uUseEmissiveMap != 0)
    {
        // Emissive textures are authored as color textures; SRGB texture upload decodes to linear here.
        emissiveLinear *= uEmissiveMap.Sample(uAlbedoSampler, emissiveTexCoord).rgb;
    }

    float3 f0 = lerp(0.04.xxx, albedo, metallic);

    float ambientOcclusion = 1.0;
    if (uUseAoMap != 0)
    {
        ambientOcclusion = uAoMap.Sample(uAoSampler, aoTexCoord).r;
    }

    float3 irradiance = EvaluateDiffuseIrradianceSh(normal);
    float3 diffuseIbl = irradiance * albedo / PI;

    float3 reflection = reflect(-viewDir, normal);
    float3 prefilteredColor = uPrefilterMap.SampleLevel(uPrefilterSampler, reflection, roughness * uMaxReflectionLod).rgb;
    float2 envBrdf = uBrdfLut.Sample(uBrdfLutSampler, float2(max(dot(normal, viewDir), 0.0), roughness)).rg;
    // SSR-03: env specular IBL must NOT be gated by sun facing; image-based reflections
    // do not depend on the shadow light's direction, and the gate made the SSR composite's
    // spec-IBL reconstruction (ssr_indirect.ps.hlsl) impossible to match.
    float3 specularIbl = prefilteredColor * (f0 * envBrdf.x + envBrdf.y);

    // Indirect: clamp mapped roughness for Fresnel stability.
    // Do not boost diffuse IBL on back faces — that removes the direct/indirect terminator.
    float nDotVGeom = max(dot(geomNormalNorm, viewDir), 0.0);
    float roughnessForIndirectEnergy = max(roughness, 0.55);
    float3 specularEnergy = FresnelSchlickRoughness(nDotVGeom, f0, roughnessForIndirectEnergy);
    float3 diffuseEnergy = (1.0.xxx - specularEnergy) * (1.0 - metallic);
    // Omit spec IBL / diffuse SH ambient when a composite will add each back (see the u*Omit flags).
    const float3 indirectSpecular = uOmitSpecularIbl != 0 ? 0.0.xxx : specularIbl;
    const float3 indirectDiffuse = uOmitDiffuseIbl != 0 ? 0.0.xxx : diffuseEnergy * diffuseIbl;
    float3 ambient = (indirectDiffuse + indirectSpecular) * uEnvironmentIntensity * ambientOcclusion;

    float3 directShadowed = 0.0.xxx;
    float3 directUnshadowed = 0.0.xxx;
    float3 directFillUnshadowed = 0.0.xxx;
    float3 directSunUnshadowed = 0.0.xxx;
    float shadowFactor = 1.0;

    [loop]
    for (int i = 0; i < uLightCount; ++i)
    {
        float3 lightDir;
        float attenuation;
        float spotIntensity;

        CalcLightDirectionAndAttenuation(
            uLightTypes[i],
            uLightPositions[i].xyz,
            uLightDirections[i].xyz,
            uLightAttenConstant[i],
            uLightAttenLinear[i],
            uLightAttenQuadratic[i],
            uLightRange[i],
            uLightInnerCutoffCos[i],
            uLightOuterCutoffCos[i],
            input.fragPos,
            lightDir,
            attenuation,
            spotIntensity);

        float3 radiance = SrgbToLinear(uLightColors[i].rgb) * uLightIntensities[i];
        float perturbedNdotL = max(dot(normal, lightDir), 0.0);
        float geomNdotL = max(dot(geomNormalNorm, lightDir), 0.0);
        float diffuseNdotL = perturbedNdotL;
        if (i == uShadowLightIndex && uLightTypes[i] == LIGHT_TYPE_DIRECTIONAL)
        {
            // Let normal maps shape sun diffuse, but keep the geometric hemisphere as the hard visibility gate.
            diffuseNdotL = geomNdotL > 0.0 ? perturbedNdotL : 0.0;
        }

        float3 contribution = CalcCookTorranceContribution(
            normal,
            viewDir,
            lightDir,
            albedo,
            roughness,
            metallic,
            radiance,
            diffuseNdotL,
            perturbedNdotL);

        float3 litContribution = contribution * attenuation * spotIntensity;

        float lightShadow = 1.0;
        if (i == uShadowLightIndex)
        {
            lightShadow = CalcShadow(input.fragPos, input.viewDepth, geomNormal, lightDir);
            shadowFactor = lightShadow;
            directSunUnshadowed += litContribution;
        }
        else
        {
            directFillUnshadowed += litContribution;
        }

        directUnshadowed += litContribution;
        directShadowed += litContribution * lightShadow;
    }

    if (uShadowLightIndex < 0)
    {
        shadowFactor = 1.0;
    }

    float3 result = ambient + directShadowed + emissiveLinear;

    if (uDebugMode != 0)
    {
        float3 debugColor = 0.0.xxx;
        if (uDebugMode == 1)
        {
            float shadow = 1.0;
            if (uShadowLightIndex >= 0)
            {
                shadow = CalcShadow(input.fragPos, input.viewDepth, geomNormal, normalize(uLightDirections[uShadowLightIndex].xyz));
            }
            debugColor = shadow.xxx;
        }
        else if (uDebugMode == 2)
        {
            debugColor = directUnshadowed / (directUnshadowed + 0.25.xxx);
        }
        else if (uDebugMode == 3)
        {
            debugColor = ambient / (ambient + 0.25.xxx);
        }
        else if (uDebugMode == 4)
        {
            int cascadeIndex = SelectCascadeForReceiverWorldPos(input.fragPos, input.viewDepth);
            float3 sampleCoords = WorldToShadowSampleCoords(cascadeIndex, input.fragPos);
            debugColor = float3(sampleCoords.xy, 0.0);
        }
        else if (uDebugMode == 5)
        {
            int cascadeIndex = SelectCascadeIndex(input.viewDepth);
            float3 sampleCoords = WorldToShadowSampleCoords(cascadeIndex, input.fragPos);
            if (sampleCoords.z >= 0.0 && sampleCoords.z <= 1.0)
            {
                // Raw stable clip Z (OpenGL-style shadow depth). No normalization — corner-based
                // ranges were saturating most of the scene to hard white/black.
                debugColor = sampleCoords.z.xxx;
            }
            else
            {
                debugColor = DebugDepthOutOfBoundsColor();
            }
        }
        else if (uDebugMode == 6)
        {
            int cascadeIndex = SelectCascadeIndex(input.viewDepth);
            float farSplit = uCascadeEndSplits[uCascadeCount - 1];
            float normDepth = saturate(
                (input.viewDepth - uCascadeNearPlane) / max(farSplit - uCascadeNearPlane, 1e-4));
            float3 cascadeColor = float3(1.0, 0.85, 0.2);
            if (cascadeIndex == 0)
            {
                cascadeColor = float3(1.0, 0.2, 0.2);
            }
            else if (cascadeIndex == 1)
            {
                cascadeColor = float3(0.2, 1.0, 0.2);
            }
            else if (cascadeIndex == 2)
            {
                cascadeColor = float3(0.2, 0.35, 1.0);
            }
            debugColor = cascadeColor * (0.25 + normDepth * 0.75);
        }
        else if (uDebugMode == 7)
        {
            debugColor = geomNormal * 0.5 + 0.5;
        }
        else if (uDebugMode == 8)
        {
            float handedness = input.tangent.w < 0.0 ? 0.15 : 0.85;
            debugColor = handedness.xxx;
        }
        else if (uDebugMode == 9)
        {
            float farSplit = uCascadeEndSplits[uCascadeCount - 1];
            float normDepth = saturate(
                (input.viewDepth - uCascadeNearPlane) / max(farSplit - uCascadeNearPlane, 1e-4));
            debugColor = normDepth.xxx;
        }
        else if (uDebugMode == 10)
        {
            debugColor = ComputeCascadeBlendFactor(input.viewDepth).xxx;
        }
        else if (uDebugMode == 11)
        {
            float3 worldNormal = normalize(geomNormal);
            float3 irradiance = EvaluateDiffuseIrradianceSh(worldNormal);
            float3 diffuseOnly = irradiance * albedo * (1.0 - metallic) / PI;
            debugColor = diffuseOnly / (diffuseOnly + 0.25.xxx);
        }
        else if (uDebugMode == 12)
        {
            float3 worldNormal = normalize(normal);
            float3 reflectionDebug = reflect(-viewDir, worldNormal);
            float3 prefilteredColor = uPrefilterMap.SampleLevel(
                uPrefilterSampler, reflectionDebug, roughness * uMaxReflectionLod).rgb;
            float2 envBrdf = uBrdfLut.Sample(
                uBrdfLutSampler, float2(max(dot(worldNormal, viewDir), 0.0), roughness)).rg;
            float3 specularOnly = prefilteredColor * (f0 * envBrdf.x + envBrdf.y);
            debugColor = specularOnly / (specularOnly + 0.25.xxx);
        }
        else if (uDebugMode == 13)
        {
            float3 geomDiffuse = 0.0.xxx;
            [loop]
            for (int lightIndex = 0; lightIndex < uLightCount; ++lightIndex)
            {
                float3 lightDir;
                float attenuation;
                float spotIntensity;
                CalcLightDirectionAndAttenuation(
                    uLightTypes[lightIndex],
                    uLightPositions[lightIndex].xyz,
                    uLightDirections[lightIndex].xyz,
                    uLightAttenConstant[lightIndex],
                    uLightAttenLinear[lightIndex],
                    uLightAttenQuadratic[lightIndex],
                    uLightRange[lightIndex],
                    uLightInnerCutoffCos[lightIndex],
                    uLightOuterCutoffCos[lightIndex],
                    input.fragPos,
                    lightDir,
                    attenuation,
                    spotIntensity);

                float3 radiance = SrgbToLinear(uLightColors[lightIndex].rgb) * uLightIntensities[lightIndex];
                float nDotL = max(dot(normalize(geomNormal), lightDir), 0.0);
                geomDiffuse += albedo * radiance * nDotL * attenuation * spotIntensity;
            }
            debugColor = geomDiffuse / (geomDiffuse + 0.25.xxx);
        }
        else if (uDebugMode == 14)
        {
            debugColor = normalize(normal) * 0.5 + 0.5;
        }
        else if (uDebugMode == 15)
        {
            float shadow = 1.0;
            if (uShadowLightIndex >= 0)
            {
                shadow = CalcShadowInternal(
                    input.fragPos,
                    input.viewDepth,
                    geomNormal,
                    normalize(uLightDirections[uShadowLightIndex].xyz),
                    false);
            }
            debugColor = shadow.xxx;
        }
        else if (uDebugMode == 16)
        {
            UnbiasedShadowSample shadowSample = BuildUnbiasedShadowSample(input.fragPos, input.viewDepth);
            if (!shadowSample.inBounds)
            {
                debugColor = DebugDepthOutOfBoundsColor();
            }
            else
            {
                float storedDepth = FetchShadowDepthRaw(shadowSample.cascadeIndex, shadowSample.shadowUv);
                debugColor = storedDepth.xxx;
            }
        }
        else if (uDebugMode == 17)
        {
            UnbiasedShadowSample shadowSample = BuildUnbiasedShadowSample(input.fragPos, input.viewDepth);
            if (!shadowSample.inBounds)
            {
                debugColor = DebugDepthOutOfBoundsColor();
            }
            else
            {
                float storedDepth = FetchShadowDepthRaw(shadowSample.cascadeIndex, shadowSample.shadowUv);
                float separation = shadowSample.receiverClipZ - storedDepth;
                float separationInBiasUnits =
                    separation / max(shadowSample.minSeparation, 1e-5);
                debugColor = saturate(separationInBiasUnits * 0.5 + 0.5).xxx;
            }
        }
        else if (uDebugMode == 20)
        {
            float sunFacing = 0.0;
            if (uShadowLightIndex >= 0)
            {
                float3 lightDir;
                float attenuation;
                float spotIntensity;
                CalcLightDirectionAndAttenuation(
                    uLightTypes[uShadowLightIndex],
                    uLightPositions[uShadowLightIndex].xyz,
                    uLightDirections[uShadowLightIndex].xyz,
                    uLightAttenConstant[uShadowLightIndex],
                    uLightAttenLinear[uShadowLightIndex],
                    uLightAttenQuadratic[uShadowLightIndex],
                    uLightRange[uShadowLightIndex],
                    uLightInnerCutoffCos[uShadowLightIndex],
                    uLightOuterCutoffCos[uShadowLightIndex],
                    input.fragPos,
                    lightDir,
                    attenuation,
                    spotIntensity);
                sunFacing = saturate(dot(normalize(geomNormal), lightDir));
            }
            debugColor = sunFacing.xxx;
        }
        else if (uDebugMode == 21)
        {
            UnbiasedShadowSample shadowSample = BuildUnbiasedShadowSample(input.fragPos, input.viewDepth);
            debugColor = shadowSample.inBounds
                ? VisualizeClipDepth(shadowSample.cascadeIndex, shadowSample.compareDepth).xxx
                : DebugDepthOutOfBoundsColor();
        }
        else if (uDebugMode == 22)
        {
            int cascadeIndex = SelectCascadeIndex(input.viewDepth);
            float3 sampleCoords = WorldToShadowSampleCoords(cascadeIndex, input.fragPos);
            if (!IsInShadowCascadeBounds(sampleCoords))
            {
                debugColor = 0.5.xxx;
            }
            else
            {
                bool blocked = IsShadowBlockedRawCenterTexel(cascadeIndex, input.fragPos);
                debugColor = blocked ? 0.0.xxx : 1.0.xxx;
            }
        }
        else
        {
            debugColor = float3(1.0, 0.0, 1.0);
        }

        output.oDirect = float4(debugColor, 1.0);
        output.oIndirect = float4(0.0, 0.0, 0.0, 0.0);
        output.oSunShadow = float4(0.0, 0.0, 0.0, 1.0);
        // SSR-02: shading normal (normal-mapped) so the SSR composite's spec-IBL recompute
        // matches the lighting above and traces pick up normal-map detail.
        output.oNormal = float4(normalize(normal), 1.0);
        output.oVelocity = float4(ComputeMotionNdc(input.currClip, input.prevClip), 0.0, 1.0);
        WriteGBufferMaterials(output, albedo, roughness, metallic, emissiveLinear);
        return output;
    }

    if (uSplitLightingOutput != 0)
    {
        output.oDirect = float4(directFillUnshadowed + emissiveLinear, 1.0);
        output.oIndirect = float4(ambient, 1.0);
        output.oSunShadow = float4(directSunUnshadowed, shadowFactor);
        // SSR-02: shading normal (normal-mapped) so the SSR composite's spec-IBL recompute
        // matches the lighting above and traces pick up normal-map detail.
        output.oNormal = float4(normalize(normal), 1.0);
        output.oVelocity = float4(ComputeMotionNdc(input.currClip, input.prevClip), 0.0, 1.0);
        WriteGBufferMaterials(output, albedo, roughness, metallic, emissiveLinear);
        return output;
    }

    if (uOutputLinear != 0)
    {
        output.oDirect = float4(result, 1.0);
        output.oIndirect = float4(0.0, 0.0, 0.0, 0.0);
    }
    else
    {
        output.oDirect = float4(LinearToSrgb(result), 1.0);
        output.oIndirect = float4(0.0, 0.0, 0.0, 0.0);
    }
    output.oSunShadow = float4(0.0, 0.0, 0.0, 1.0);
    // SSR-02: shading normal (normal-mapped) so the SSR composite's spec-IBL recompute
    // matches the lighting above and traces pick up normal-map detail.
    output.oNormal = float4(normalize(normal), 1.0);
    output.oVelocity = float4(ComputeMotionNdc(input.currClip, input.prevClip), 0.0, 1.0);
    WriteGBufferMaterials(output, albedo, roughness, metallic, emissiveLinear);
    return output;
}
