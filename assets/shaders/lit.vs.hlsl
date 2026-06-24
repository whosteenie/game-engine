#include "hlsl_common.hlsl"

cbuffer PerVertex : register(b0)
{
    float4x4 uModel;
    float4x4 uView;
    float4x4 uProjection;
    float4x4 uLightSpaceMatrix;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 texCoord0 : TEXCOORD0;
    float2 texCoord1 : TEXCOORD1;
    float4 tangent : TANGENT;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 fragPos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 texCoord0 : TEXCOORD2;
    float2 texCoord1 : TEXCOORD3;
    float4 tangent : TEXCOORD4;
    float4 fragPosLightSpace : TEXCOORD5;
    float viewDepth : TEXCOORD6;
};

VSOutput main(VSInput input)
{
    VSOutput output;

    float4 worldPos = mul(uModel, float4(input.position, 1.0));
    output.fragPos = worldPos.xyz;

    float3x3 normalMatrix = NormalMatrixFromModel(uModel);
    output.normal = mul(normalMatrix, input.normal);
    output.tangent = float4(normalize(mul(normalMatrix, input.tangent.xyz)), input.tangent.w);
    output.texCoord0 = input.texCoord0;
    output.texCoord1 = input.texCoord1;

    float4 viewPos = mul(uView, worldPos);
    output.viewDepth = viewPos.z;

    output.fragPosLightSpace = mul(uLightSpaceMatrix, worldPos);
    output.position = mul(uProjection, viewPos);

    return output;
}
