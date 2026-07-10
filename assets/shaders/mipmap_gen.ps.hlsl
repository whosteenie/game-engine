Texture2D uSource : register(t0);

SamplerState uSourceSampler : register(s0);

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

float4 main(PSInput input) : SV_Target
{
    const float2 texelSize = float2(uTexelSizeX, uTexelSizeY);
    const float2 uv = input.texCoord;

    float3 color = 0.0;
    color += uSource.Sample(uSourceSampler, uv + float2(-0.25, -0.25) * texelSize).rgb;
    color += uSource.Sample(uSourceSampler, uv + float2(0.25, -0.25) * texelSize).rgb;
    color += uSource.Sample(uSourceSampler, uv + float2(-0.25, 0.25) * texelSize).rgb;
    color += uSource.Sample(uSourceSampler, uv + float2(0.25, 0.25) * texelSize).rgb;
    return float4(color * 0.25, 1.0);
}
