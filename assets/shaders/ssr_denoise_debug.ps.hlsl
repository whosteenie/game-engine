Texture2D uTraceMap : register(t0);
Texture2D uDepthMap : register(t1);

SamplerState uTraceSampler : register(s0);
SamplerState uDepthSampler : register(s1);

cbuffer PerPixel : register(b0)
{
    int uDebugMode;
    float uDebugScale;
    float2 _pad0;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

// 0 = rgb, 1 = confidence (alpha)
float4 main(PSInput input) : SV_Target
{
    const float depth = uDepthMap.Sample(uDepthSampler, input.texCoord).r;
    if (depth >= 0.9999)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    const float4 trace = uTraceMap.Sample(uTraceSampler, input.texCoord);
    if (uDebugMode == 1)
    {
        const float confidence = saturate(trace.a);
        return float4(confidence, confidence, confidence, 1.0);
    }

    return float4(max(trace.rgb * uDebugScale, 0.0.xxx), 1.0);
}
