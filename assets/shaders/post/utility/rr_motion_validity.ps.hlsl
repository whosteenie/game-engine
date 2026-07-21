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
    const float2 physicalMotion = uMotion.SampleLevel(uPointSampler, input.texCoord, 0);
    // The canonical validity mask means no previous pixel owns this current signal. Preserve the
    // physical vector only where that correspondence exists. For rejection, change only the axis
    // nearest a frame edge and use the minimum vector that places history two texels out of bounds.
    // This remains unambiguously invalid across temporal jitter without injecting the previous
    // multi-screen (4,4) motion spike into RR's reconstruction neighborhood.
    if (invalid <= 0.5)
    {
        return physicalMotion;
    }

    uint width;
    uint height;
    uMotion.GetDimensions(width, height);
    const float2 uv = input.texCoord;
    const float2 marginUv = 2.0 / max(float2(width, height), 1.0.xx);
    const float left = uv.x;
    const float right = 1.0 - uv.x;
    const float top = uv.y;
    const float bottom = 1.0 - uv.y;

    float2 rejectedMotion = physicalMotion;
    if (min(left, right) <= min(top, bottom))
    {
        // previousUv.x = uv.x - 0.5 * motion.x
        rejectedMotion.x = left <= right
            ? 2.0 * (left + marginUv.x)
            : -2.0 * (right + marginUv.x);
    }
    else
    {
        // previousUv.y = uv.y + 0.5 * motion.y
        rejectedMotion.y = top <= bottom
            ? -2.0 * (top + marginUv.y)
            : 2.0 * (bottom + marginUv.y);
    }
    return rejectedMotion;
}
