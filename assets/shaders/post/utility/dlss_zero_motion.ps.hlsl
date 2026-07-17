struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float2 main(PSInput input) : SV_Target
{
    return float2(0.0, 0.0);
}
