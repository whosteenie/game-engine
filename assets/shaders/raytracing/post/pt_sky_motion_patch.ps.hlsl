// Real-time PT: merge raster motion (geometry) with path-tracer sky motion (camera misses).
// Raster sky_background does not write RT4, so sky pixels keep MV=0 and DLSS smears the PT sky.

Texture2D<float4> uRasterMotion : register(t0);
Texture2D<float4> uPtMotion : register(t1);
Texture2D<uint2> uPtMetadata : register(t2);

SamplerState uPointSampler : register(s0)
{
    Filter = MIN_MAG_MIP_POINT;
    AddressU = CLAMP;
    AddressV = CLAMP;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

struct PSOutput
{
    float2 oMotion : SV_Target0;
};

PSOutput main(PSInput input)
{
    PSOutput output;
    const int2 pixel = int2(input.position.xy);
    const uint2 meta = uPtMetadata.Load(int3(pixel, 0));
    const float4 raster = uRasterMotion.Sample(uPointSampler, input.texCoord);
    const float4 pt = uPtMotion.Sample(uPointSampler, input.texCoord);

    // Metadata.r = instanceId + 1; 0 => primary camera ray missed (sky).
    output.oMotion = (meta.x == 0u) ? pt.xy : raster.xy;
    return output;
}
