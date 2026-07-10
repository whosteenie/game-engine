// Phase P3/P4 — convert reference accumulation sum to per-pixel mean HDR for display/DLSS.

Texture2D<float4> uAccumSum : register(t0);

cbuffer PerPixel : register(b0)
{
    uint uSampleCount;
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
    const float3 sum = uAccumSum.Load(int3(pixel, 0)).rgb;
    return float4(sum / max(uSampleCount, 1u), 1.0);
}
