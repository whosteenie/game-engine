// Phase P3 path-tracer reference accumulation (devdoc/dxr-path-tracing.md).
// Adds the current frame's HDR radiance into a running sum (RGBA32F). Reset on any history change.

Texture2D<float4> uCurrentFrame : register(t0);
Texture2D<float4> uAccumSum : register(t1);

cbuffer PerPixel : register(b0)
{
    int uReset;
    float3 _Padding0;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target
{
    const uint2 pixel = uint2(input.position.xy);
    const float3 current = uCurrentFrame.Load(int3(pixel, 0)).rgb;

    if (uReset != 0)
    {
        return float4(current, 1.0);
    }

    const float3 previous = uAccumSum.Load(int3(pixel, 0)).rgb;
    return float4(previous + current, 1.0);
}
