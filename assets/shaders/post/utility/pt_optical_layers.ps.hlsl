Texture2D uFullOrReflection : register(t0);
Texture2D uTransmission : register(t1);

SamplerState uLinearSampler : register(s0);

cbuffer OpticalLayerParams : register(b0)
{
    int uComposite;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target
{
    const float4 first = uFullOrReflection.Sample(uLinearSampler, input.texCoord);
    const float4 transmission = uTransmission.Sample(uLinearSampler, input.texCoord);
    if (uComposite == 0)
    {
        return float4(max(first.rgb - transmission.rgb, 0.0.xxx), first.a);
    }
    if (uComposite == 1)
    {
        return float4(first.rgb + transmission.rgb, 1.0);
    }
    if (uComposite == 2)
    {
        const float3 positive = max(first.rgb, 0.0.xxx);
        return float4(positive / (1.0 + positive), 1.0);
    }

    // Diagnostic delta is second - first. Yellow means RR made the layer brighter, cyan means
    // darker, and black means close agreement. Four-times luminance gain makes weak glass-layer
    // history errors visible without changing either reconstruction input.
    const float deltaLuminance = dot(
        transmission.rgb - first.rgb,
        float3(0.2126, 0.7152, 0.0722));
    const float positiveDelta = saturate(deltaLuminance * 4.0);
    const float negativeDelta = saturate(-deltaLuminance * 4.0);
    return float4(positiveDelta, positiveDelta + negativeDelta, negativeDelta, 1.0);
}
