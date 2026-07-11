#ifndef MESH_LIGHTING_HLSLI
#define MESH_LIGHTING_HLSLI

// Forward lighting for the mesh-shader G-buffer path.
//
// The BRDF / cascaded-shadow / SH-irradiance math is SHARED with the classic lit path via
// pbr_lighting_core.hlsli (included below) — one source of truth for both. This file only declares
// the mesh path's own 16-byte-aligned constant buffer + t7/t8/t9 resources (so the C++ mirror is
// unambiguous) and `#define`-aliases them onto the canonical pbr.ps symbol names the shared core
// expects. The aliases MUST precede the include. The mesh-specific glue (surface assembly + the
// per-light split-lighting loop) lives after the include. See
// devdoc/rendering/mesh-instancing-and-mesh-shaders.md.

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


// Shared BRDF / cascaded-shadow / SH-irradiance math (aliased above onto this cbuffer).
#include "pbr_lighting_core.hlsli"

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

        float3 radiance = SrgbToLinear(light.color.rgb) * light.params0.x;
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
