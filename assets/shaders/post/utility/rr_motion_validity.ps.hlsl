Texture2D<float2> uMotion : register(t0);
Texture2D<float> uValidityMask : register(t1);

SamplerState uPointSampler : register(s0)
{
    Filter = MIN_MAG_MIP_POINT;
    AddressU = CLAMP;
    AddressV = CLAMP;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float2 main(PSInput input) : SV_Target
{
    const float invalid = uValidityMask.SampleLevel(uPointSampler, input.texCoord, 0);
    // The canonical validity mask means no previous pixel owns this current signal. Preserve the
    // physical vector only where that correspondence exists; otherwise encode no correspondence
    // as a lookup more than two complete render extents outside history.
    return invalid > 0.5
        ? float2(4.0, 4.0)
        : uMotion.SampleLevel(uPointSampler, input.texCoord, 0);
}
