TextureCube uSkyboxMap : register(t0);
SamplerState uSkyboxSampler : register(s0);

cbuffer PerPixel : register(b0)
{
    float uExposure;
    int uSplitLightingOutput;
};

struct PSInput
{
    float4 position : SV_Position;
    float3 localPos : TEXCOORD0;
};

struct PSOutput
{
    float4 oDirect : SV_Target0;
    float4 oIndirect : SV_Target1;
    float4 oNormal : SV_Target2;
    float4 oSunShadow : SV_Target3;
};

PSOutput main(PSInput input)
{
    PSOutput output;
    float3 direction = normalize(input.localPos);
    float3 color = uSkyboxMap.Sample(uSkyboxSampler, direction).rgb * uExposure;

    output.oNormal = float4(0.0, 0.0, 1.0, 1.0);
    output.oSunShadow = float4(0.0, 0.0, 0.0, 1.0);

    if (uSplitLightingOutput != 0)
    {
        output.oDirect = float4(0.0, 0.0, 0.0, 0.0);
        output.oIndirect = float4(color, 1.0);
        return output;
    }

    output.oDirect = float4(color, 1.0);
    output.oIndirect = float4(0.0, 0.0, 0.0, 0.0);
    return output;
}
