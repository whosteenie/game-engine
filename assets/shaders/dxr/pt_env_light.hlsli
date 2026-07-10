#ifndef PT_ENV_LIGHT_HLSLI
#define PT_ENV_LIGHT_HLSLI

#include "../environment_sampling.hlsl"

// Path-tracer-only bindings (S5 / F2 environment importance sampling).
Texture2D<float4> g_EnvEquirectMap : register(t17);
StructuredBuffer<float> g_EnvImportanceCdf : register(t16);

float3 EquirectUvToDirection(float2 uv)
{
    const float phi = (uv.x - 0.5) * 2.0 * kPi;
    const float y = sin(kPi * (uv.y - 0.5));
    const float horiz = sqrt(max(1.0 - y * y, 0.0));
    return normalize(float3(cos(phi) * horiz, y, sin(phi) * horiz));
}

float EnvIsCosLatitude(float v)
{
    return max(cos(kPi * (v - 0.5)), 1e-6);
}

float EnvIsSolidAnglePerCell(float cosLat)
{
    return (2.0 * kPi / float(g_EnvIsCdfWidth))
        * (kPi / float(g_EnvIsCdfHeight))
        * cosLat;
}

uint EnvIsFindCellIndex(float u)
{
    uint lo = 0u;
    uint hi = g_EnvLightImportanceCount;
    while (lo + 1u < hi)
    {
        const uint mid = (lo + hi) >> 1u;
        if (g_EnvImportanceCdf[mid] <= u)
        {
            lo = mid;
        }
        else
        {
            hi = mid;
        }
    }
    return min(lo, g_EnvLightImportanceCount - 1u);
}

float3 SampleEnvEquirectRadiance(float3 direction)
{
    const float2 uv = DirectionToEquirectUv(normalize(direction));
    return g_EnvEquirectMap.SampleLevel(g_LinearClampSampler, uv, 0.0).rgb * g_EnvironmentIntensity;
}

// When the analytic sun is active, it owns its angular cone — the HDR still contains a sun disk,
// so env NEE must not re-light that region (double sun + mismatched shadow directions).
bool AnalyticSunActiveForEnvNee()
{
    return g_SunIntensity > 1e-4;
}

float AnalyticSunCosConeBoundary()
{
    const float tanR = max(g_SunAngularTanRadius, 1e-6);
    return rsqrt(1.0 + tanR * tanR);
}

bool IsInAnalyticSunCone(float3 wi)
{
    if (!AnalyticSunActiveForEnvNee())
    {
        return false;
    }
    const float3 sunL = normalize(g_SunDirection);
    return dot(normalize(wi), sunL) >= AnalyticSunCosConeBoundary();
}

float3 EnvNeeRadiance(float3 wi)
{
    if (IsInAnalyticSunCone(wi))
    {
        return 0.0.xxx;
    }
    return SampleEnvEquirectRadiance(wi);
}

bool SampleEnvLightDirection(float4 xi, out float3 wi, out float pdfSolidAngle)
{
    wi = float3(0.0, 1.0, 0.0);
    pdfSolidAngle = 0.0;

    if (g_EnvLightImportanceCount == 0u || g_EnvLightImportanceInvWeightSum <= 0.0)
    {
        return false;
    }

    const uint cellIndex = EnvIsFindCellIndex(xi.x);
    const uint cdfW = g_EnvIsCdfWidth;
    const uint cdfH = g_EnvIsCdfHeight;
    const uint ix = cellIndex % cdfW;
    const uint iy = cellIndex / cdfW;

    const float u = (float(ix) + xi.y) / float(cdfW);
    const float v = (float(iy) + xi.z) / float(cdfH);
    wi = EquirectUvToDirection(float2(u, v));

    const float cosLat = EnvIsCosLatitude(v);
    const float3 radiance = EnvNeeRadiance(wi);
    const float lum = dot(radiance, float3(0.2126, 0.7152, 0.0722));
    const float weight = lum * cosLat;
    const float solidAngle = EnvIsSolidAnglePerCell(cosLat);
    pdfSolidAngle = weight * g_EnvLightImportanceInvWeightSum / max(solidAngle, 1e-8);
    return pdfSolidAngle > 0.0;
}

#endif
