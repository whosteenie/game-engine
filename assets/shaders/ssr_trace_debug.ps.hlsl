Texture2D uTraceMap : register(t0);
Texture2D uDepthMap : register(t1);

SamplerState uTraceSampler : register(s0);
SamplerState uDepthSampler : register(s1);

cbuffer PerPixel : register(b0)
{
    int uSsrTraceDebugMode;
    float3 _pad;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

// 0 = trace rgb, 1 = confidence (alpha)
float4 main(PSInput input) : SV_Target
{
    const float depth = uDepthMap.Sample(uDepthSampler, input.texCoord).r;
    if (depth >= 0.9999)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    const float4 trace = uTraceMap.Sample(uTraceSampler, input.texCoord);
    if (uSsrTraceDebugMode == 0)
    {
        return float4(trace.rgb, 1.0);
    }

    const float confidence = trace.a;
    return float4(confidence, confidence, confidence, 1.0);
}
