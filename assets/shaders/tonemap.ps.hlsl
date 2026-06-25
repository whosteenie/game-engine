Texture2D uHdrColor : register(t0);
Texture2D uBloom : register(t1);

SamplerState uHdrColorSampler : register(s0);
SamplerState uBloomSampler : register(s1);

cbuffer PerPixel : register(b0)
{
    float uExposure;
    int uTonemapMode;
    int uUseBloom;
    float uBloomIntensity;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float3 LinearToSrgb(float3 linearColor)
{
    return pow(max(linearColor, 0.0.xxx), 1.0.xxx / 2.2);
}

float3 ACESFilm(float3 color)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((color * (a * color + b)) / (color * (c * color + d) + e));
}

float3 Reinhard(float3 color)
{
    const float whitePoint = 4.0;
    float3 numerator = color * (1.0 + color / (whitePoint * whitePoint));
    return numerator / (1.0 + color);
}

float InterleavedGradientNoise(float2 fragCoord)
{
    float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    return frac(magic.z * frac(dot(fragCoord, magic.xy)));
}

float4 main(PSInput input) : SV_Target
{
    float3 hdr = uHdrColor.Sample(uHdrColorSampler, input.texCoord).rgb * exp2(uExposure);

    if (uUseBloom != 0)
    {
        hdr += uBloom.Sample(uBloomSampler, input.texCoord).rgb * uBloomIntensity * exp2(uExposure);
    }

    float3 mapped;

    if (uTonemapMode == 1)
    {
        mapped = Reinhard(hdr);
    }
    else if (uTonemapMode == 2)
    {
        mapped = ACESFilm(hdr);
    }
    else
    {
        mapped = hdr;
    }

    mapped = LinearToSrgb(mapped);

    uint width;
    uint height;
    uHdrColor.GetDimensions(width, height);
    float2 fragCoord = input.texCoord * float2(width, height);
    float dither = InterleavedGradientNoise(fragCoord) - 0.5;
    mapped += dither / 255.0;

    return float4(saturate(mapped), 1.0);
}
