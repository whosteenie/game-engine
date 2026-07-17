Texture2D uInput : register(t0);

SamplerState uInputSampler : register(s0);

cbuffer PerPixel : register(b0)
{
    float uTexelSizeX;
    float uTexelSizeY;
    float uThreshold;
    float _pad0;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float Luma(float3 rgb)
{
    return dot(rgb, float3(0.2126, 0.7152, 0.0722));
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

    const float centerLuma = Luma(center);
    const float deltaLeft = abs(centerLuma - Luma(left));
    const float deltaRight = abs(centerLuma - Luma(right));
    const float deltaUp = abs(centerLuma - Luma(up));
    const float deltaDown = abs(centerLuma - Luma(down));

    const float edgeHorz = deltaLeft + deltaRight;
    const float edgeVert = deltaUp + deltaDown;
    const float edge = max(edgeHorz, edgeVert);

    float2 edges = float2(0.0, 0.0);
    if (edge > uThreshold)
    {
        edges.x = edgeHorz >= edgeVert ? 1.0 : 0.0;
        edges.y = edgeHorz < edgeVert ? 1.0 : 0.0;
    }

    return float4(edges, 0.0, 1.0);
}
