#include "environment_sampling.hlsl"

Texture2D uEquirectMap : register(t0);
SamplerState uEquirectSampler : register(s0);

cbuffer PerPixel : register(b0)
{
    float4x4 uInvProjection;
    float4x4 uInvView;
    float uExposure;
    float uRotationY;
    int uSplitLightingOutput;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

struct PSOutput
{
    float4 oDirect : SV_Target0;
    float4 oIndirect : SV_Target1;
    float4 oNormal : SV_Target2;
    float4 oSunShadow : SV_Target3;
};

float3 ViewRayFromUv(float2 texCoord)
{
    float2 clipXY = float2(texCoord.x * 2.0 - 1.0, 1.0 - texCoord.y * 2.0);
    float4 viewFar = mul(uInvProjection, float4(clipXY, 1.0, 1.0));
    float3 viewDir = normalize(viewFar.xyz / viewFar.w);
    return normalize(mul((float3x3)uInvView, viewDir));
}

PSOutput main(PSInput input)
{
    PSOutput output;
    float3 worldDir = ViewRayFromUv(input.texCoord);
    float3 color = SampleEquirectEnvironment(
        uEquirectMap,
        uEquirectSampler,
        worldDir,
        uRotationY,
        uExposure);

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
