cbuffer PerVertex : register(b0)
{
    float4x4 uModel;
    float4x4 uLightSpaceMatrix;
    float3 uLightDirectionTowardSource;
    float uCasterDepthBias;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
};

struct VSOutput
{
    float4 position : SV_Position;
};

VSOutput main(VSInput input)
{
    VSOutput output;

    float4 clip = mul(uLightSpaceMatrix, mul(uModel, float4(input.position, 1.0)));

    // Optional caster-side slope bias. The default scale is zero; keep this explicit so contact
    // behavior is controlled by the renderer tuning value rather than hidden rasterizer state.
    float3 normalWorld = normalize(mul((float3x3)uModel, input.normal));
    float nDotL = dot(normalWorld, normalize(uLightDirectionTowardSource));
    float sinTheta = (nDotL > 0.0) ? sqrt(saturate(1.0 - nDotL * nDotL)) : 0.0;
    clip.z += uCasterDepthBias * sinTheta * clip.w;

    output.position = clip;
    return output;
}
