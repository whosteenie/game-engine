cbuffer PerPixel : register(b0)
{
    float3 uColor;
    float uFadeStart;
    float3 uCameraPosition;
    float uFadeEnd;
    float uCellSize;
    float uMaxRenderDistance;
    float uMajorInterval;
    float4x4 uView;
    int uOutputLinear;
    int uSplitLightingOutput;
};

struct PSInput
{
    float4 position : SV_Position;
    float3 worldPos : TEXCOORD0;
};

struct PSOutput
{
    float4 oDirect : SV_Target0;
    float4 oIndirect : SV_Target1;
    float4 oNormal : SV_Target2;
    float4 oSunShadow : SV_Target3;
};

float3 LinearToSrgb(float3 linearColor)
{
    return pow(max(linearColor, 0.0.xxx), 1.0.xxx / 2.2);
}

float LineAlpha(float2 uv, float lodCellScale)
{
    float2 grid = abs(frac(uv - 0.5) - 0.5);
    float2 fw = max(fwidth(uv), 0.0001.xx);
    float2 lines = grid / fw;
    float lineMask = 1.0 - clamp(min(lines.x, lines.y), 0.0, 1.0);

    // LOD in units of the coarser grid cell so minor/major fade at the same world distance.
    float cellsPerPixel = max(fw.x, fw.y);
    float pixelsPerLodCell = (1.0 / cellsPerPixel) * lodCellScale;
    float lodFade = smoothstep(0.85, 2.2, pixelsPerLodCell);

    return lineMask * lodFade;
}

PSOutput main(PSInput input)
{
    PSOutput output;

    float distXZ = length(input.worldPos.xz - uCameraPosition.xz);
    if (distXZ > uMaxRenderDistance)
    {
        discard;
    }

    float2 xz = input.worldPos.xz;
    float2 minorUv = xz / uCellSize;
    float2 majorUv = xz / (uCellSize * uMajorInterval);

    float minor = LineAlpha(minorUv, uMajorInterval);
    float major = LineAlpha(majorUv, 1.0);

    float axisMask = max(
        1.0 - clamp(abs(xz.x) / max(fwidth(xz.x) * 1.5, 0.0001), 0.0, 1.0),
        1.0 - clamp(abs(xz.y) / max(fwidth(xz.y) * 1.5, 0.0001), 0.0, 1.0));

    float lineStrength = max(max(minor * 0.45, major * 0.75), axisMask * minor * 0.9);

    float3 viewDir = normalize(uCameraPosition - input.worldPos);
    float grazeFadeNear = smoothstep(0.015, 0.12, abs(viewDir.y));
    float nearGrazeWeight = 1.0 - smoothstep(uFadeStart * 0.2, uFadeStart * 0.75, distXZ);
    float grazeFade = lerp(1.0, grazeFadeNear, nearGrazeWeight);

    float distFade = 1.0 - smoothstep(uFadeStart, uFadeEnd, distXZ);

    // Only suppress horizon stacking in the outer distance band (screen-space LOD handles the rest).
    float horizonView = smoothstep(0.008, 0.07, abs(viewDir.y));
    float outerBand = smoothstep(uFadeEnd * 0.78, uFadeEnd, distXZ);
    float horizonFade = lerp(1.0, horizonView, outerBand);

    float alpha = lineStrength * grazeFade * distFade * horizonFade;
    clip(alpha - 0.02);

    float3 linearColor = pow(uColor, 2.2.xxx);
    float3 viewNormal = float3(0.0, 1.0, 0.0);
    output.oNormal = float4(viewNormal, 1.0);

    if (uSplitLightingOutput != 0)
    {
        output.oDirect = float4(linearColor, alpha);
        output.oIndirect = float4(0.0, 0.0, 0.0, 0.0);
        output.oSunShadow = float4(0.0, 0.0, 0.0, 1.0);
        return output;
    }

    if (uOutputLinear != 0)
    {
        output.oDirect = float4(linearColor, alpha);
    }
    else
    {
        output.oDirect = float4(LinearToSrgb(linearColor), alpha);
    }
    output.oIndirect = float4(0.0, 0.0, 0.0, 0.0);
    output.oSunShadow = float4(0.0, 0.0, 0.0, 1.0);
    return output;
}
