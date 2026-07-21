Texture2D uSceneColorMap : register(t0);
Texture2D uDepthMap : register(t1);

SamplerState uSceneColorSampler : register(s0);
SamplerState uDepthSampler : register(s1);

cbuffer PerPixel : register(b0)
{
    int uSsrDebugMode;
    float3 _pad;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

// 0 = scene color rgb, 1 = validity mask (alpha)
float4 main(PSInput input) : SV_Target
{
    const float depth = uDepthMap.Sample(uDepthSampler, input.texCoord).r;
    if (depth >= 0.9999)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    const float4 sceneColor = uSceneColorMap.Sample(uSceneColorSampler, input.texCoord);
    if (uSsrDebugMode == 0)
    {
        return float4(sceneColor.rgb, 1.0);
    }

    const float validity = sceneColor.a;
    return float4(validity, validity, validity, 1.0);
}
