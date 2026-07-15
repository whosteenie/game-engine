// Raw PT motion-vector reprojection audit. This deliberately bypasses DLSS/RR and compares the
// current input radiance against last frame's raw radiance at the UV implied by the exact motion
// texture and scale handed to Streamline.

Texture2D<float4> uCurrentRadiance : register(t0);
Texture2D<float4> uPreviousRadiance : register(t1);
Texture2D<float2> uMotion : register(t2);

SamplerState uCurrentSampler : register(s0);
SamplerState uPreviousSampler : register(s1);
SamplerState uMotionSampler : register(s2);

cbuffer PerPixel : register(b0)
{
    int uPreviousFrameValid;
    float uMotionScaleX;
    float uMotionScaleY;
    float uDifferenceGain;
    float uTexelSizeX;
    float uTexelSizeY;
    float uResidualThreshold;
    float _Padding0;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float Luminance(float3 color)
{
    return dot(max(color, 0.0.xxx), float3(0.2126, 0.7152, 0.0722));
}

float3 Heat(float value)
{
    const float v = saturate(value);
    return saturate(float3(2.0 * v, 2.0 * v - 0.65, 1.25 * v - 1.0));
}

float4 main(PSInput input) : SV_Target
{
    if (uPreviousFrameValid == 0)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    const float2 currentUv = input.texCoord;
    const float2 motion = uMotion.Sample(uMotionSampler, currentUv);
    // Streamline's mvec scale maps our current-minus-previous NDC vector to the prior UV.
    const float2 previousUv = currentUv + motion * float2(uMotionScaleX, uMotionScaleY);
    if (any(previousUv < 0.0.xx) || any(previousUv > 1.0.xx))
    {
        // Blue is ordinary off-screen disocclusion, not a guide mismatch.
        return float4(0.03, 0.18, 0.95, 1.0);
    }

    // A small corresponding footprint removes independent one-sample PT noise from this audit.
    // It intentionally does not cross a large radius: a bad motion contract must remain visible
    // at the object scale where its DLSS/RR ghost is observed.
    float currentLuma = 0.0;
    float previousLuma = 0.0;
    [unroll]
    for (int y = -2; y <= 2; ++y)
    {
        [unroll]
        for (int x = -2; x <= 2; ++x)
        {
            const float2 offset = float2(x * uTexelSizeX, y * uTexelSizeY);
            currentLuma += Luminance(uCurrentRadiance.Sample(uCurrentSampler, currentUv + offset).rgb);
            previousLuma += Luminance(uPreviousRadiance.Sample(uPreviousSampler, previousUv + offset).rgb);
        }
    }
    currentLuma *= 1.0 / 25.0;
    previousLuma *= 1.0 / 25.0;
    // Exposure-aware residual keeps a bright emissive readable instead of saturating it.
    const float residual = abs(currentLuma - previousLuma)
        / max(max(currentLuma, previousLuma), 0.25);
    const float structuralResidual = max(residual - uResidualThreshold, 0.0);
    return float4(Heat(structuralResidual * uDifferenceGain), 1.0);
}
