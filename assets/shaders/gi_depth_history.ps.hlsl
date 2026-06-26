Texture2D uDepth : register(t0);

SamplerState uDepthSampler : register(s0);

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

// Store hardware depth from the jittered depth buffer for world-space disocclusion next frame.
float4 main(PSInput input) : SV_Target
{
    const float depth = uDepth.Sample(uDepthSampler, input.texCoord).r;
    return float4(depth, 0.0, 0.0, 0.0);
}
