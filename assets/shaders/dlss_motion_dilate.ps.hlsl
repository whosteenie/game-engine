Texture2D uDepth : register(t0);
Texture2D uMotion : register(t1);

SamplerState uDepthSampler : register(s0);
SamplerState uMotionSampler : register(s1);

cbuffer PerPixel : register(b0)
{
    float uTexelSizeX;
    float uTexelSizeY;
    float _pad0;
    float _pad1;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

// DLSS expects dilated vectors to assign foreground motion to disoccluded background pixels. With
// conventional [0,1] hardware depth, the smallest depth in a 3x3 footprint is the foreground.
float4 main(PSInput input) : SV_Target
{
    float closestDepth = 2.0;
    float2 closestMotion = 0.0.xx;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float2 uv = input.texCoord + float2(x * uTexelSizeX, y * uTexelSizeY);
            const float depth = uDepth.Sample(uDepthSampler, uv).r;
            if (depth < closestDepth)
            {
                closestDepth = depth;
                closestMotion = uMotion.Sample(uMotionSampler, uv).xy;
            }
        }
    }
    return float4(closestMotion, 0.0, 1.0);
}
