// RT reflection resolve — the "denoised" debug view and the D6 composite primitive.
// NRD RELAX only produces trustworthy radiance for pixels that hit geometry within the
// denoising range. Miss / sky pixels (the ray reached the environment) are not denoised;
// leaving them to NRD leaves stale, reprojected history that smears the skybox under camera
// motion. The trace already writes the correct, noise-free environment radiance into the raw
// buffer every frame, so we select:
//   hit  -> denoised radiance
//   miss -> fresh raw environment radiance
// keyed on the RAW hit distance (RELAX packing: alpha = world-space hit distance, miss =
// maxTraceDistance). This is exactly the gate the D6 composite will use to fall back to IBL.

Texture2D uDenoised : register(t0);
Texture2D uRaw : register(t1);
SamplerState uSampler : register(s0);

cbuffer ResolveParams : register(b0)
{
    // Valid UV region of the inputs (quality-scaled DXR outputs write only the top-left
    // dispatchSize/textureSize region). <= 0 means full texture.
    float2 uUvScale;
    // World-space trace range; miss pixels report hit distance == maxTraceDistance.
    float uMaxTraceDistance;
    float _pad;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target
{
    const float2 uvScale = uUvScale.x <= 0.0 ? float2(1.0, 1.0) : uUvScale;
    const float2 uv = input.texCoord * uvScale;

    const float4 raw = uRaw.Sample(uSampler, uv);
    const float4 denoised = uDenoised.Sample(uSampler, uv);

    // Miss / sky: raw hit distance sits at (or beyond) the trace range. Use the fresh env
    // radiance from the raw buffer so the skybox does not accumulate reprojected history.
    const float missThreshold = uMaxTraceDistance > 0.0 ? uMaxTraceDistance * 0.999 : 1e30;
    const bool isMiss = raw.a >= missThreshold;

    const float3 color = isMiss ? raw.rgb : denoised.rgb;
    return float4(color, 1.0);
}
