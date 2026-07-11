#include "mesh_lighting.hlsli"

static const uint INVALID_INDEX = 0xFFFFFFFFu;
static const uint MATERIAL_FLAG_METALLIC_ROUGHNESS_MAP = 1u;

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
    uint instanceId : TEXCOORD9;
    uint materialId : TEXCOORD10;
};

struct MaterialGpu
{
    float3 albedo;
    float metallic;
    float3 emissive;
    float roughness;
    uint albedoTexIndex;
    uint albedoUvOffsetFloats;
    uint normalTexIndex;
    uint normalUvOffsetFloats;
    uint roughnessTexIndex;
    uint roughnessUvOffsetFloats;
    uint emissiveTexIndex;
    uint emissiveUvOffsetFloats;
    uint flags;
    float transmission;
    float indexOfRefraction;
    float thinWalled;
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

StructuredBuffer<MaterialGpu> gMaterials : register(t5);
Texture2D<float4> gBindlessTextures[] : register(t0, space1);
SamplerState gLinearWrapSampler : register(s0);

float2 ComputeMotionNdc(float4 currClip, float4 prevClip)
{
    float2 currNdc = currClip.xy / currClip.w;
    float2 prevNdc = prevClip.xy / prevClip.w;
    return currNdc - prevNdc;
}

float2 SelectUv(uint uvOffsetFloats, float2 uv0, float2 uv1)
{
    return uvOffsetFloats == 8u ? uv1 : uv0;
}

float3 CalcNormalFromMap(float3 worldNormal, float4 tangent, float2 uv, uint normalTexIndex)
{
    float3 tangentNormal = gBindlessTextures[normalTexIndex].Sample(gLinearWrapSampler, uv).xyz * 2.0 - 1.0;
    float3 n = normalize(worldNormal);
    float3 t = normalize(tangent.xyz);
    t = normalize(t - n * dot(n, t));
    float3 b = normalize(cross(n, t)) * tangent.w;
    float3x3 tbn = float3x3(t, b, n);
    return normalize(mul(tangentNormal, tbn));
}

PSOutput main(PSInput input)
{
    PSOutput output;
    const MaterialGpu material = gMaterials[input.materialId];

    float3 normal = normalize(input.normal);
    if (material.normalTexIndex != INVALID_INDEX)
    {
        normal = CalcNormalFromMap(
            normal,
            input.tangent,
            SelectUv(material.normalUvOffsetFloats, input.texCoord0, input.texCoord1),
            material.normalTexIndex);
    }

    float3 albedo = saturate(material.albedo);
    if (material.albedoTexIndex != INVALID_INDEX)
    {
        albedo *= gBindlessTextures[material.albedoTexIndex]
            .Sample(gLinearWrapSampler, SelectUv(material.albedoUvOffsetFloats, input.texCoord0, input.texCoord1))
            .rgb;
    }

    float roughness = saturate(material.roughness);
    float metallic = saturate(material.metallic);
    if (material.roughnessTexIndex != INVALID_INDEX)
    {
        const float3 roughnessSample = gBindlessTextures[material.roughnessTexIndex]
            .Sample(gLinearWrapSampler, SelectUv(material.roughnessUvOffsetFloats, input.texCoord0, input.texCoord1))
            .rgb;
        if ((material.flags & MATERIAL_FLAG_METALLIC_ROUGHNESS_MAP) != 0)
        {
            roughness *= roughnessSample.g;
            metallic *= roughnessSample.b;
        }
        else
        {
            roughness *= roughnessSample.r;
        }
    }
    roughness = saturate(roughness);
    metallic = saturate(metallic);

    float3 emissive = max(material.emissive, 0.0.xxx);
    if (material.emissiveTexIndex != INVALID_INDEX)
    {
        emissive *= gBindlessTextures[material.emissiveTexIndex]
            .Sample(gLinearWrapSampler, SelectUv(material.emissiveUvOffsetFloats, input.texCoord0, input.texCoord1))
            .rgb;
    }

    MeshLightingSurface surface;
    surface.worldPos = input.fragPos;
    surface.shadingNormal = normal;
    surface.geomNormal = input.normal;
    surface.albedo = albedo;
    surface.roughness = roughness;
    surface.metallic = metallic;
    surface.emissive = emissive;
    surface.viewDepth = input.viewDepth;
    const MeshLightingResult lighting = ComputeMeshSplitLighting(surface);

    output.oDirect = float4(lighting.direct, 1.0);
    output.oIndirect = float4(lighting.indirect, 1.0);
    output.oNormal = float4(normalize(normal), 1.0);
    output.oSunShadow = float4(lighting.sunUnshadowed, lighting.shadowFactor);
    output.oVelocity = float4(ComputeMotionNdc(input.currClip, input.prevClip), 0.0, 1.0);
    output.oMaterial0 = float4(albedo, roughness);
    output.oMaterial1 = float4(metallic, emissive);
    return output;
}
