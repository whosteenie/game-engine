#include "../common/hlsl_common.hlsl"

cbuffer PerVertex : register(b0)
{
    float4x4 uModel;
    float4x4 uView;
    float4x4 uProjection;
    float uOutlineWidth;
    float uOutlineWidthWorld;
    float uRadialExpand;
    float uViewportHeight;
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

    float3x3 normalMatrix = NormalMatrixFromModel(uModel);
    float3 worldNormal = normalize(mul(normalMatrix, input.normal));
    float4 worldPosition = mul(uModel, float4(input.position, 1.0));

    if (uOutlineWidth > 0.0)
    {
        float3 modelOrigin = uModel[3].xyz;
        float3 fromOrigin = worldPosition.xyz - modelOrigin;
        float originDistance = length(fromOrigin);
        if (originDistance > 0.0001)
        {
            worldPosition.xyz = modelOrigin + fromOrigin * (1.0 + uRadialExpand);
        }

        worldPosition.xyz += worldNormal * uOutlineWidthWorld;

        float4 clipPosition = mul(uProjection, mul(uView, float4(worldPosition.xyz, 1.0)));
        float3 viewNormal = normalize(mul((float3x3)uView, worldNormal));
        float3 clipNormal = mul((float3x3)uProjection, viewNormal);

        float2 offset = clipNormal.xy;
        float offsetLength = length(offset);
        if (offsetLength > 0.0001)
        {
            offset /= offsetLength;
        }

        float pixelToClip = (2.0 * uOutlineWidth) / max(uViewportHeight, 1.0);
        clipPosition.xy += offset * pixelToClip * clipPosition.w;
        output.position = clipPosition;
        return output;
    }

    output.position = mul(uProjection, mul(uView, worldPosition));
    return output;
}
