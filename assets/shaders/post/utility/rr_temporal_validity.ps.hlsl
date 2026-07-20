Texture2D<float> uCurrentDepth : register(t0);
Texture2D<float4> uCurrentNormalRoughness : register(t1);
Texture2D<uint> uCurrentOwner : register(t2);
Texture2D<float2> uMotion : register(t3);
Texture2D<float> uPreviousDepth : register(t4);
Texture2D<float4> uPreviousNormalRoughness : register(t5);
Texture2D<uint> uPreviousOwner : register(t6);

SamplerState uPointSampler : register(s0);

cbuffer PerFrame : register(b0)
{
    float4x4 uClipToPrevClip;
    float uHistoryValid;
    float uDepthRelativeThreshold;
    float uNormalDotThreshold;
    float uDiagnosticOutput;
    float uMvecScaleX;
    float uMvecScaleY;
    float2 _padding;
    float2 uCurrentJitterNdc;
    float2 uPreviousJitterNdc;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target
{
    const float2 uv = input.texCoord;
    float4 rejection = 0.0.xxxx;
    float geometryHasMotion = 0.0;
    if (uHistoryValid < 0.5)
    {
        rejection.r = 1.0;
    }
    else
    {
        const float2 motion = uMotion.SampleLevel(uPointSampler, uv, 0).xy;
        uint motionWidth;
        uint motionHeight;
        uMotion.GetDimensions(motionWidth, motionHeight);
        const float2 motionPixels = motion
            * float2(uMvecScaleX * motionWidth, uMvecScaleY * motionHeight);
        // Owner/depth/normal disagreement is a disocclusion only when this represented receiver
        // moves between frames. Static disagreement is stochastic guide variation and rejecting
        // it through required motion destroys RR accumulation.
        geometryHasMotion = any(abs(motionPixels) > 0.01.xx) ? 1.0 : 0.0;
        // Motion is deliberately unjittered, while both guide bundles were sampled with their
        // frame's projection jitter. Move from the current jittered pixel lattice to the previous
        // jittered lattice after applying current-to-previous motion.
        const float2 jitterDeltaUv =
            (uPreviousJitterNdc - uCurrentJitterNdc) * float2(0.5, -0.5);
        const float2 previousUv =
            uv + motion * float2(uMvecScaleX, uMvecScaleY) + jitterDeltaUv;
        if (any(previousUv < 0.0.xx) || any(previousUv > 1.0.xx)
            || !all(isfinite(previousUv)))
        {
            rejection.r = 1.0;
        }
        else
        {
            uint ownerWidth;
            uint ownerHeight;
            uCurrentOwner.GetDimensions(ownerWidth, ownerHeight);
            const int2 currentOwnerPixel = min(
                int2(uv * float2(ownerWidth, ownerHeight)),
                int2(ownerWidth - 1u, ownerHeight - 1u));
            const int2 previousOwnerPixel = min(
                int2(previousUv * float2(ownerWidth, ownerHeight)),
                int2(ownerWidth - 1u, ownerHeight - 1u));
            const uint currentOwner = uCurrentOwner.Load(int3(currentOwnerPixel, 0));
            const uint previousOwner = uPreviousOwner.Load(int3(previousOwnerPixel, 0));
            rejection.g = currentOwner != previousOwner ? 1.0 : 0.0;

            const float currentDepth = uCurrentDepth.SampleLevel(uPointSampler, uv, 0);
            // History stores 1-deviceDepth in fp16.  The complement retains relative precision
            // near the far plane where storing nonlinear device depth directly produces bands.
            const float previousDepth = 1.0
                - uPreviousDepth.SampleLevel(uPointSampler, previousUv, 0);
            const bool currentSky = currentDepth >= 0.99999;
            const bool previousSky = previousDepth >= 0.99999;
            if (currentSky != previousSky)
            {
                rejection.b = 1.0;
            }
            else if (!currentSky)
            {
                // Current and previous view-space depths cannot be compared directly under camera
                // motion. Transform the current surface into the previous clip domain and compare
                // the depth it should have had there with the depth actually stored at history UV.
                // clipToPrevClip is an unjittered transform. Remove the current sampling offset
                // before reconstructing the current homogeneous point in that domain.
                const float2 currentNdc =
                    float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0) - uCurrentJitterNdc;
                const float4 expectedPreviousClip = mul(
                    uClipToPrevClip, float4(currentNdc, currentDepth, 1.0));
                if (!all(isfinite(expectedPreviousClip)) || expectedPreviousClip.w <= 1e-6)
                {
                    rejection.r = 1.0;
                }
                else
                {
                    const float expectedPreviousDepth =
                        expectedPreviousClip.z / expectedPreviousClip.w;
                    if (!isfinite(expectedPreviousDepth)
                        || expectedPreviousDepth < 0.0 || expectedPreviousDepth > 1.0)
                    {
                        rejection.r = 1.0;
                    }
                    else
                    {
                        // Perspective device depth is reciprocal in view distance. Comparing its
                        // remaining range makes this a relative geometric threshold across depth.
                        const float relativeDelta = abs(expectedPreviousDepth - previousDepth)
                            / max(
                                max(1.0 - expectedPreviousDepth, 1.0 - previousDepth),
                                1e-5);
                        rejection.b = relativeDelta > uDepthRelativeThreshold ? 1.0 : 0.0;
                    }
                }
            }

            const float3 currentNormal = normalize(
                uCurrentNormalRoughness.SampleLevel(uPointSampler, uv, 0).xyz);
            const float3 previousNormal = normalize(
                uPreviousNormalRoughness.SampleLevel(uPointSampler, previousUv, 0).xyz);
            if (!currentSky && !previousSky)
            {
                rejection.a = dot(currentNormal, previousNormal) < uNormalDotThreshold ? 1.0 : 0.0;
            }
        }
    }

    // Invalid history/UV is unconditional. Geometry/path disagreement is a disocclusion only
    // while the represented receiver moves; static guide variation remains accumulable.
    const float selectedRejection = max(
        rejection.r,
        max(max(rejection.g, rejection.b), rejection.a) * geometryHasMotion);
    return uDiagnosticOutput > 0.5 ? rejection : selectedRejection.xxxx;
}
