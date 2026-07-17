Texture2D uInput : register(t0);
SamplerState uInputSampler : register(s0);

cbuffer PerPixel : register(b0)
{
    int uOutputRgb;
    int uOutputAlpha; // grayscale of .a (RT reflection hit-distance view); wins over uOutputRgb
    // Valid UV region of the input (quality-scaled DXR outputs write only the top-left
    // dispatchSize/textureSize region). <= 0 means "unset" (legacy callers) -> full texture.
    float2 uUvScale;
    // Multiplier applied to .a before display (e.g. 1/maxTraceDistance to normalize hit
    // distance). <= 0 means "unset" -> 1.
    float uAlphaScale;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target
{
    const float2 uvScale = uUvScale.x <= 0.0 ? float2(1.0, 1.0) : uUvScale;
    float4 sampled = uInput.Sample(uInputSampler, input.texCoord * uvScale);
    if (uOutputAlpha != 0)
    {
        const float alphaScale = uAlphaScale <= 0.0 ? 1.0 : uAlphaScale;
        return float4(saturate(sampled.a * alphaScale).xxx, 1.0);
    }

    if (uOutputRgb != 0)
    {
        return float4(sampled.rgb, 1.0);
    }

    return float4(sampled.r, sampled.r, sampled.r, 1.0);
}
