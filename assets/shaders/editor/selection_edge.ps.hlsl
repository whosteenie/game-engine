Texture2D uMask : register(t0);
SamplerState uMaskSampler : register(s0);

cbuffer PerPixel : register(b0)
{
    float2 uTexelSize;
    float uOutlineWidth;
    float _pad0;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float ComputeEdgeAlpha(float2 uv)
{
    const int radius = 4;
    float dilated = 0.0;
    float eroded = 1.0;

    [loop]
    for (int y = -radius; y <= radius; ++y)
    {
        [loop]
        for (int x = -radius; x <= radius; ++x)
        {
            float dist = length(float2((float)x, (float)y));
            if (dist > uOutlineWidth + 0.5)
            {
                continue;
            }

            float2 sampleUv = uv + float2((float)x, (float)y) * uTexelSize;
            float maskSample = uMask.Sample(uMaskSampler, sampleUv).r;
            dilated = max(dilated, maskSample);
            eroded = min(eroded, maskSample);
        }
    }

    float edge = dilated - eroded;
    return smoothstep(0.12, 0.88, edge);
}

float4 main(PSInput input) : SV_Target
{
    float alpha = ComputeEdgeAlpha(input.texCoord);
    return float4(alpha.xxx, alpha);
}
