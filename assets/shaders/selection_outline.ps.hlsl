cbuffer PerPixel : register(b0)
{
    float3 uColor;
    float _pad0;
};

struct PSInput
{
    float4 position : SV_Position;
};

float4 main(PSInput input) : SV_Target
{
    return float4(uColor, 1.0);
}
