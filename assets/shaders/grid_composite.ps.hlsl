Texture2D uGridOverlay : register(t0);
SamplerState uGridOverlaySampler : register(s0);

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target
{
    return uGridOverlay.Sample(uGridOverlaySampler, input.texCoord);
}
