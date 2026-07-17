// Stores the depth source selected for DLSS/RR into a color history target. PT's actual DLSS D24
// input is resolved from this source and is not shader-readable; this preserves the same depth
// signal before that lossless-for-diagnostic conversion.

Texture2D<float> uDepth : register(t0);
SamplerState uDepthSampler : register(s0);

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target
{
    return float4(uDepth.Sample(uDepthSampler, input.texCoord), 0.0, 0.0, 1.0);
}
