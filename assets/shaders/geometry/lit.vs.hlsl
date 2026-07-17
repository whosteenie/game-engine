#include "../common/hlsl_common.hlsl"

// Motion-vector convention (SSGI Phase 1):
//   velocity.xy = currentNDC.xy - previousNDC.xy
// where NDC is clip.xy / clip.w from unjittered projection matrices.
// Stored in RG16F attachment; sky/background pixels remain 0 (cleared).

cbuffer PerVertex : register(b0)
{
    float4x4 uModel;
    float4x4 uPrevModel;
    float4x4 uView;
    float4x4 uPrevView;
    float4x4 uProjection;
    float4x4 uUnjitteredProjection;
    float4x4 uPrevUnjitteredProjection;
    float4x4 uLightSpaceMatrix;
    float uTemporalHistoryValid;
    float3 _motionPad;
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
    float4 currClip : TEXCOORD7;
    float4 prevClip : TEXCOORD8;
};

VSOutput main(VSInput input)
{
    VSOutput output;

    float4 worldPos = mul(uModel, float4(input.position, 1.0));
    output.fragPos = worldPos.xyz;

    float3x3 normalMatrix = NormalMatrixFromModel(uModel);
    output.normal = mul(normalMatrix, input.normal);

    float tangentHandedness = input.tangent.w;
    if (determinant((float3x3)uModel) < 0.0)
    {
        tangentHandedness = -tangentHandedness;
    }
    output.tangent = float4(normalize(mul(normalMatrix, input.tangent.xyz)), tangentHandedness);
    output.texCoord0 = input.texCoord0;
    output.texCoord1 = input.texCoord1;

    float4 viewPos = mul(uView, worldPos);
    output.viewDepth = viewPos.z;

    output.fragPosLightSpace = mul(uLightSpaceMatrix, worldPos);
    output.position = mul(uProjection, viewPos);

    output.currClip = mul(uUnjitteredProjection, viewPos);
    if (uTemporalHistoryValid > 0.5)
    {
        float4 prevWorldPos = mul(uPrevModel, float4(input.position, 1.0));
        float4 prevViewPos = mul(uPrevView, prevWorldPos);
        output.prevClip = mul(uPrevUnjitteredProjection, prevViewPos);
    }
    else
    {
        output.prevClip = output.currClip;
    }

    return output;
}
