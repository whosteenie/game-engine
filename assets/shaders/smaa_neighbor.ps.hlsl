Texture2D uInput : register(t0);
Texture2D uEdges : register(t1);

SamplerState uInputSampler : register(s0);
SamplerState uEdgesSampler : register(s1);

cbuffer PerPixel : register(b0)
{
    float uTexelSizeX;
    float uTexelSizeY;
    float uSearchSteps;
    float _pad0;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target
{
    const float2 texelSize = float2(uTexelSizeX, uTexelSizeY);
    const float2 uv = input.texCoord;
    const float3 center = uInput.Sample(uInputSampler, uv).rgb;
    const float2 edgeMask = uEdges.Sample(uEdgesSampler, uv).rg;

    if (edgeMask.x + edgeMask.y < 0.5)
    {
        return float4(center, 1.0);
    }

    const bool horizontal = edgeMask.x > edgeMask.y;
    const float2 offset = horizontal ? float2(texelSize.x, 0.0) : float2(0.0, texelSize.y);

    float3 sum = center;
    float weight = 1.0;
    const int steps = max(1, (int)uSearchSteps);

    [loop]
    for (int i = 1; i <= steps; ++i)
    {
        const float t = (float)i;
        const float2 uvA = uv + offset * t;
        const float2 uvB = uv - offset * t;
        const float edgeA = uEdges.Sample(uEdgesSampler, uvA).r + uEdges.Sample(uEdgesSampler, uvA).g;
        const float edgeB = uEdges.Sample(uEdgesSampler, uvB).r + uEdges.Sample(uEdgesSampler, uvB).g;
        const float wA = saturate(1.0 - edgeA);
        const float wB = saturate(1.0 - edgeB);
        sum += uInput.Sample(uInputSampler, uvA).rgb * wA;
        sum += uInput.Sample(uInputSampler, uvB).rgb * wB;
        weight += wA + wB;
    }

    return float4(sum / weight, 1.0);
}
