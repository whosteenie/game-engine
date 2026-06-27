cbuffer PerPixel : register(b0)
{
    float3 uColor;
    float uFadeStart;
    float3 uCameraPosition;
    float uFadeEnd;
    float uCellSize;
    float4x4 uView;
    float uMajorInterval;
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

float LineAlpha(float2 uv)
{
    float2 grid = abs(frac(uv - 0.5) - 0.5);
    float2 fw = max(fwidth(uv), 0.0001.xx);
    float2 lines = grid / fw;
    return 1.0 - clamp(min(lines.x, lines.y), 0.0, 1.0);
}

PSOutput main(PSInput input)
{
    PSOutput output;

    float2 xz = input.worldPos.xz;
    float2 minorUv = xz / uCellSize;
    float2 majorUv = xz / (uCellSize * uMajorInterval);

    float minor = LineAlpha(minorUv);
    float major = LineAlpha(majorUv);

    float axisMask = max(
        1.0 - clamp(abs(xz.x) / max(fwidth(xz.x) * 1.5, 0.0001), 0.0, 1.0),
        1.0 - clamp(abs(xz.y) / max(fwidth(xz.y) * 1.5, 0.0001), 0.0, 1.0));

    float lineStrength = max(max(minor * 0.45, major * 0.75), axisMask * minor * 0.9);

    float3 viewDir = normalize(uCameraPosition - input.worldPos);
    float distXZ = length(input.worldPos.xz - uCameraPosition.xz);
    float grazeFadeNear = smoothstep(0.015, 0.12, abs(viewDir.y));
    // Grazing-angle fade is only needed up close; at distance it erases lines over coplanar floor geometry.
    float nearGrazeWeight = 1.0 - smoothstep(uFadeStart * 0.2, uFadeStart * 0.75, distXZ);
    float grazeFade = lerp(1.0, grazeFadeNear, nearGrazeWeight);

    float distFade = 1.0 - smoothstep(uFadeStart, uFadeEnd, distXZ);

    float alpha = lineStrength * grazeFade * distFade;
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
