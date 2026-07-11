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
    float4 albedoRoughness : TEXCOORD9;
    float metallic : TEXCOORD10;
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

float2 ComputeMotionNdc(float4 currClip, float4 prevClip)
{
    float2 currNdc = currClip.xy / currClip.w;
    float2 prevNdc = prevClip.xy / prevClip.w;
    return currNdc - prevNdc;
}

PSOutput main(PSInput input)
{
    PSOutput output;
    const float3 normal = normalize(input.normal);
    const float3 albedo = saturate(input.albedoRoughness.rgb);
    const float roughness = saturate(input.albedoRoughness.a);
    const float metallic = saturate(input.metallic);

    // G2 smoke path: intentionally minimal lighting/material behavior. Full PBR parity moves to
    // the MaterialGpu table path once scene instances are the raster source of truth.
    const float ndotl = saturate(dot(normal, normalize(float3(-0.35, 0.8, -0.45)))) * 0.75 + 0.25;
    output.oDirect = float4(albedo * ndotl, 1.0);
    output.oIndirect = float4(albedo * 0.04, 1.0);
    output.oNormal = float4(normal * 0.5 + 0.5, roughness);
    output.oSunShadow = float4(1.0, 1.0, 1.0, 1.0);
    output.oVelocity = float4(ComputeMotionNdc(input.currClip, input.prevClip), 0.0, 0.0);
    output.oMaterial0 = float4(albedo, roughness);
    output.oMaterial1 = float4(metallic, 0.0, 0.0, 0.0);
    return output;
}
