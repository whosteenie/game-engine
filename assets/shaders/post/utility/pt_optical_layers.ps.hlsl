Texture2D uFullOrReflection : register(t0);
Texture2D uTransmission : register(t1);
Texture2D uPsrThroughput : register(t2);

SamplerState uLinearSampler : register(s0);

cbuffer OpticalLayerParams : register(b0)
{
    int uComposite;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target
{
    const float4 first = uFullOrReflection.Sample(uLinearSampler, input.texCoord);
    const float4 transmission = uTransmission.Sample(uLinearSampler, input.texCoord);
    if (uComposite == 0)
    {
        return float4(max(first.rgb - transmission.rgb, 0.0.xxx), first.a);
    }
    if (uComposite == 1)
    {
        return float4(first.rgb + transmission.rgb, 1.0);
    }
    if (uComposite == 2)
    {
        const float3 positive = max(first.rgb, 0.0.xxx);
        return float4(positive / (1.0 + positive), 1.0);
    }
    if (uComposite >= 4 && uComposite <= 7)
    {
        // Render-resolution preprocessing uses point ownership and throughput so the one RR input
        // never mixes primary and PSR domains. Display-resolution composition uses linear sampling
        // as a compact edge-aware upsample; alpha gates the remodulation at mirror silhouettes.
        uint width;
        uint height;
        uPsrThroughput.GetDimensions(width, height);
        const int2 sourcePixel = clamp(
            int2(input.texCoord * float2(width, height)),
            int2(0, 0),
            int2(width - 1, height - 1));
        const float4 psrPoint = uPsrThroughput.Load(int3(sourcePixel, 0));
        const float4 psrLinear = uPsrThroughput.Sample(uLinearSampler, input.texCoord);
        const bool preprocess = uComposite == 4 || uComposite == 6;
        const float4 psr = preprocess ? psrPoint : psrLinear;
        const float owner = preprocess ? (psr.a >= 0.5 ? 1.0 : 0.0) : saturate(psr.a);
        const float3 throughput = max(psr.rgb, 0.0.xxx);

        if (preprocess)
        {
            // The path tracer stores the separated transmission lobe in receiver space. For a PSR
            // glass receiver, convert it to physical space before subtracting it from the physical
            // full signal, then demodulate the remaining reflection lobe exactly once.
            const float3 physicalTransmission = transmission.rgb * lerp(1.0.xxx, throughput, owner);
            const float3 physical = max(
                first.rgb - (uComposite == 6 ? physicalTransmission : 0.0.xxx),
                0.0.xxx);
            const float3 epsilon = 1.0e-3.xxx;
            const float3 receiver = float3(
                throughput.r > epsilon.r ? physical.r / throughput.r : 0.0,
                throughput.g > epsilon.g ? physical.g / throughput.g : 0.0,
                throughput.b > epsilon.b ? physical.b / throughput.b : 0.0);
            // Bound demodulation independently of the firefly clamp. Zero-throughput channels are
            // defined as zero receiver contribution and can never produce NaN/Inf or hue leakage.
            return float4(lerp(physical, min(receiver, 65504.0.xxx), owner), first.a);
        }

        const float3 remodulated = lerp(first.rgb, first.rgb * throughput, owner);
        const float3 remodulatedTransmission = lerp(
            max(transmission.rgb, 0.0.xxx),
            max(transmission.rgb, 0.0.xxx) * throughput,
            owner);
        const float3 combined = remodulated
            + (uComposite == 7 ? remodulatedTransmission : 0.0.xxx);
        return float4(max(combined, 0.0.xxx), 1.0);
    }

    // Diagnostic delta is second - first. Yellow means RR made the layer brighter, cyan means
    // darker, and black means close agreement. Four-times luminance gain makes weak glass-layer
    // history errors visible without changing either reconstruction input.
    const float deltaLuminance = dot(
        transmission.rgb - first.rgb,
        float3(0.2126, 0.7152, 0.0722));
    const float positiveDelta = saturate(deltaLuminance * 4.0);
    const float negativeDelta = saturate(-deltaLuminance * 4.0);
    return float4(positiveDelta, positiveDelta + negativeDelta, negativeDelta, 1.0);
}
