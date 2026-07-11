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

#include "pbr_lighting_core.hlsli"

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
    roughness = clamp(roughness, 0.0, 1.0);
    metallic = clamp(metallic, 0.0, 1.0);
    // GGX NDF/geometry need a non-zero alpha; keep the authored roughness (incl. 0) in the G-buffer.
    const float roughnessBrdf = max(roughness, 0.04);

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
            roughnessBrdf,
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
