Texture2D<float4> uPrimaryOutput : register(t0);
Texture2D<uint2> uPrimaryMetadata : register(t1);

cbuffer PerPixel : register(b0)
{
    int uViewMode;
    float uMaxTraceDistance;
    float2 _Padding0;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float3 FalseColorInstanceId(uint id)
{
    const float golden = 0.6180339887;
    const float h = frac(float(id + 1) * golden);
    const float3 rgb = abs(frac(h + float3(0.0, 0.333, 0.667)) * 6.0 - 3.0) - 1.0;
    return saturate(rgb) * 0.85 + 0.15;
}

float4 main(PSInput input) : SV_Target
{
    const uint2 pixel = uint2(input.position.xy);
    const float4 primary = uPrimaryOutput.Load(int3(pixel, 0));
    const uint2 metadata = uPrimaryMetadata.Load(int3(pixel, 0));

    if (uViewMode == 0)
    {
        if (metadata.x == 0)
        {
            return float4(0.05, 0.05, 0.85, 1.0);
        }

        return float4(FalseColorInstanceId(metadata.x - 1), 1.0);
    }

    if (uViewMode == 1)
    {
        const float depthNorm = saturate(primary.w / max(uMaxTraceDistance, 1e-4));
        return float4(depthNorm, depthNorm, depthNorm, 1.0);
    }

    if (metadata.x == 0)
    {
        return float4(0.05, 0.05, 0.85, 1.0);
    }

    return float4(primary.xyz * 0.5 + 0.5, 1.0);
}
