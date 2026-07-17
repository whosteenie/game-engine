Texture2D<float4> uMotion : register(t0);

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

// Streamline 2.12 DLSS-RR section 4.1.6 accepts RG16_FLOAT or RG32_FLOAT motion.
// Preserve the authoritative NDC current-minus-previous XY values while dropping unused ZW.
float2 main(PSInput input) : SV_Target
{
    return uMotion.Sample(uPointSampler, input.texCoord).xy;
}
