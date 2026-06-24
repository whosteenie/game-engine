Texture2D uHdrColor : register(t0);
SamplerState uHdrColorSampler : register(s0);

cbuffer PerPixel : register(b0)
{
    float uThreshold;
    float uSoftKnee;
    float _pad0;
    float _pad1;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float3 ExtractBrightColor(float3 color, float threshold, float knee)
{
    float brightness = max(color.r, max(color.g, color.b));
    float kneeRange = threshold * knee;
    float soft = brightness - threshold + kneeRange;
    soft = clamp(soft, 0.0, 2.0 * kneeRange);
    soft = (soft * soft) / (4.0 * kneeRange + 0.00001);
    float contribution = max(soft, brightness - threshold);
    contribution /= max(brightness, 0.00001);
    return color * contribution;
}

float3 main(PSInput input) : SV_Target
{
    float3 hdr = uHdrColor.Sample(uHdrColorSampler, input.texCoord).rgb;
    return ExtractBrightColor(hdr, uThreshold, uSoftKnee);
}
