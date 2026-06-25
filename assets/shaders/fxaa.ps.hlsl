Texture2D uInput : register(t0);

SamplerState uInputSampler : register(s0);

cbuffer PerPixel : register(b0)
{
    float uTexelSizeX;
    float uTexelSizeY;
    float uSubpixQuality;
    float uEdgeThreshold;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float Luma(float3 rgb)
{
    return dot(rgb, float3(0.299, 0.587, 0.114));
}

float4 main(PSInput input) : SV_Target
{
    const float2 texelSize = float2(uTexelSizeX, uTexelSizeY);
    const float2 uv = input.texCoord;

    const float3 center = uInput.Sample(uInputSampler, uv).rgb;
    const float3 left = uInput.Sample(uInputSampler, uv + float2(-texelSize.x, 0.0)).rgb;
    const float3 right = uInput.Sample(uInputSampler, uv + float2(texelSize.x, 0.0)).rgb;
    const float3 up = uInput.Sample(uInputSampler, uv + float2(0.0, -texelSize.y)).rgb;
    const float3 down = uInput.Sample(uInputSampler, uv + float2(0.0, texelSize.y)).rgb;
    const float3 upLeft = uInput.Sample(uInputSampler, uv + float2(-texelSize.x, -texelSize.y)).rgb;
    const float3 upRight = uInput.Sample(uInputSampler, uv + float2(texelSize.x, -texelSize.y)).rgb;
    const float3 downLeft = uInput.Sample(uInputSampler, uv + float2(-texelSize.x, texelSize.y)).rgb;
    const float3 downRight = uInput.Sample(uInputSampler, uv + float2(texelSize.x, texelSize.y)).rgb;

    const float centerLuma = Luma(center);
    const float edge =
        abs(centerLuma - Luma(left))
        + abs(centerLuma - Luma(right))
        + abs(centerLuma - Luma(up))
        + abs(centerLuma - Luma(down))
        + abs(centerLuma - Luma(upLeft))
        + abs(centerLuma - Luma(upRight))
        + abs(centerLuma - Luma(downLeft))
        + abs(centerLuma - Luma(downRight));

    const float threshold = max(uEdgeThreshold, 1.0 / 255.0);
    float edgeBlend = saturate((edge - threshold) / threshold);

    // Horizontal/vertical edge direction for asymmetric filtering.
    const float horizontal =
        abs(centerLuma - Luma(left)) + abs(centerLuma - Luma(right))
        + abs(Luma(upLeft) - Luma(upRight)) + abs(Luma(downLeft) - Luma(downRight));
    const float vertical =
        abs(centerLuma - Luma(up)) + abs(centerLuma - Luma(down))
        + abs(Luma(upLeft) - Luma(downLeft)) + abs(Luma(upRight) - Luma(downRight));

    float3 filtered = center;
    if (horizontal >= vertical)
    {
        filtered = (left + center + right) / 3.0;
    }
    else
    {
        filtered = (up + center + down) / 3.0;
    }

    const float3 neighborhood = (left + right + up + down + center) / 5.0;
    const float subpixBlend = saturate(uSubpixQuality * (threshold - edge) / threshold);
    filtered = lerp(filtered, neighborhood, subpixBlend);

    const float blend = saturate(edgeBlend + subpixBlend * 0.5);
    return float4(lerp(center, filtered, blend), 1.0);
}
