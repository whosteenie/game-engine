Texture2D uInput : register(t0);
SamplerState uInputSampler : register(s0);

cbuffer PerPixel : register(b0)
{
    float uDirectionX;
    float uDirectionY;
    float uBlurRadius;
    float _pad0;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float3 main(PSInput input) : SV_Target
{
    float2 direction = float2(uDirectionX, uDirectionY) * uBlurRadius;
    float3 result = uInput.Sample(uInputSampler, input.texCoord).rgb * 0.227027;
    result += uInput.Sample(uInputSampler, input.texCoord + direction * 1.0).rgb * 0.1945946;
    result += uInput.Sample(uInputSampler, input.texCoord - direction * 1.0).rgb * 0.1945946;
    result += uInput.Sample(uInputSampler, input.texCoord + direction * 2.0).rgb * 0.1216216;
    result += uInput.Sample(uInputSampler, input.texCoord - direction * 2.0).rgb * 0.1216216;
    result += uInput.Sample(uInputSampler, input.texCoord + direction * 3.0).rgb * 0.054054;
    result += uInput.Sample(uInputSampler, input.texCoord - direction * 3.0).rgb * 0.054054;
    result += uInput.Sample(uInputSampler, input.texCoord + direction * 4.0).rgb * 0.016216;
    result += uInput.Sample(uInputSampler, input.texCoord - direction * 4.0).rgb * 0.016216;
    return result;
}
