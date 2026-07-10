Texture2DMS<float> uMsaaDepth : register(t0);

cbuffer PerPixel : register(b0)
{
    uint uSampleCount;
    uint3 _pad;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

void main(PSInput input, out float outDepth : SV_Depth)
{
    const int2 pixel = int2(input.position.xy);
    float resolvedDepth = 1.0;
    [loop]
    for (uint sampleIndex = 0; sampleIndex < uSampleCount; ++sampleIndex)
    {
        resolvedDepth = min(resolvedDepth, uMsaaDepth.Load(pixel, sampleIndex));
    }
    outDepth = resolvedDepth;
}
