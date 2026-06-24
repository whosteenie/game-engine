Texture2D uInput : register(t0);
SamplerState uInputSampler : register(s0);

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

float main(PSInput input) : SV_Target
{
    float2 texelSize = float2(uTexelSizeX, uTexelSizeY);
    float result = 0.0;

    [loop]
    for (int x = -1; x <= 1; ++x)
    {
        [loop]
        for (int y = -1; y <= 1; ++y)
        {
            float2 offset = float2((float)x, (float)y) * texelSize;
            result += uInput.Sample(uInputSampler, input.texCoord + offset).r;
        }
    }

    return result / 9.0;
}
