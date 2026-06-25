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

    // Optional front-face-only slope bias. Back faces (used when front-face culling is on) keep
    // zero shader push so flat soles stay in contact with the floor in mode 22.
    float3 normalWorld = normalize(mul((float3x3)uModel, input.normal));
    float nDotL = dot(normalWorld, normalize(uLightDirectionTowardSource));
    float sinTheta = (nDotL > 0.0) ? sqrt(saturate(1.0 - nDotL * nDotL)) : 0.0;
    clip.z += uCasterDepthBias * sinTheta * clip.w;

    output.position = clip;
    return output;
}
