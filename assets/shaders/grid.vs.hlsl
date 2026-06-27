cbuffer PerVertex : register(b0)
{
    float4x4 uView;
    float4x4 uProjection;
    float2 uGridSnapOrigin;
    float uGridHeight;
    float uClipDepthBiasUlp;
};

struct VSInput
{
    float3 position : POSITION;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 worldPos : TEXCOORD0;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.worldPos = float3(
        input.position.x + uGridSnapOrigin.x,
        uGridHeight,
        input.position.z + uGridSnapOrigin.y);
    float4 clipPos = mul(uProjection, mul(uView, float4(output.worldPos, 1.0)));

    // Pull the grid slightly toward the near plane in NDC to avoid z-fighting with coplanar floor
    // geometry. Scale bias up for far fragments where depth precision is coarse.
    const float ndcDepth = clipPos.z / max(abs(clipPos.w), 1e-6);
    const float farDepthWeight = smoothstep(0.35, 0.98, ndcDepth);
    const float biasUlp = uClipDepthBiasUlp * (1.0 + farDepthWeight * 12.0);
    clipPos.z -= (biasUlp / 16777216.0) * clipPos.w;

    output.position = clipPos;
    return output;
}
