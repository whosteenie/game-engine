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

// Directional pdf of the CDF sampler for a cell: P(cell) / cell-solid-angle. P(cell) is read straight
// from the prefix-sum CDF (the exact discrete probability the sampler draws that cell with), so this
// is the ACTUAL sampling density. The old code re-derived it as lum(dir)*cosLat*invSum/Ω, which (1)
// included g_EnvironmentIntensity so it cancelled out of every NEE contribution, (2) ignored the CDF's
// 95th-percentile luminance clamp so it lost energy in the brightest cells, and (3) used pointwise
// luminance vs the cell-constant density (C4).
float EnvNeeCellPdf(uint cellIndex)
{
    if (cellIndex >= g_EnvLightImportanceCount)
    {
        return 0.0;
    }
    const float cellProb = g_EnvImportanceCdf[cellIndex + 1u] - g_EnvImportanceCdf[cellIndex];
    const uint iy = cellIndex / g_EnvIsCdfWidth;
    const float v = (float(iy) + 0.5) / float(g_EnvIsCdfHeight);
    const float solidAngle = EnvIsSolidAnglePerCell(EnvIsCosLatitude(v));
    return cellProb / max(solidAngle, 1e-8);
}

float3 SampleEnvEquirectRadiance(float3 direction)
{
    const float2 uv = DirectionToEquirectUv(normalize(direction));
    return g_EnvEquirectMap.SampleLevel(g_LinearClampSampler, uv, 0.0).rgb * g_EnvironmentIntensity;
}

bool AnalyticSunActiveForEnvNee();

float3 ClampEmbeddedSunForTransport(float3 radiance)
{
    if (!AnalyticSunActiveForEnvNee() || g_EnvDirectLightingLuminanceClamp <= 0.0)
    {
        return radiance;
    }

    const float luminance = Luminance(radiance);
    const float scale = min(1.0, g_EnvDirectLightingLuminanceClamp / max(luminance, 1e-6));
    return radiance * scale;
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
    return ClampEmbeddedSunForTransport(SampleEnvEquirectRadiance(wi));
}

// Env NEE sampling density along an arbitrary direction — the MIS partner the BSDF-sampling (miss)
// side needs. Returns 0 inside the analytic sun cone: NEE deposits no radiance there (EnvNeeRadiance
// zeroes it), so BSDF sampling owns that region and its miss-add must take full weight. Outside the
// cone this matches the pdf SampleEnvLightDirection reports for the same cell.
float EnvNeePdfForDirection(float3 dir)
{
    if (g_EnvLightImportanceCount == 0u || IsInAnalyticSunCone(dir))
    {
        return 0.0;
    }
    const float2 uv = DirectionToEquirectUv(normalize(dir));
    const uint ix = min(uint(saturate(uv.x) * float(g_EnvIsCdfWidth)), g_EnvIsCdfWidth - 1u);
    const uint iy = min(uint(saturate(uv.y) * float(g_EnvIsCdfHeight)), g_EnvIsCdfHeight - 1u);
    return EnvNeeCellPdf(iy * g_EnvIsCdfWidth + ix);
}

bool SampleEnvLightDirection(float4 xi, out float3 wi, out float pdfSolidAngle)
{
    wi = float3(0.0, 1.0, 0.0);
    pdfSolidAngle = 0.0;

    if (g_EnvLightImportanceCount == 0u)
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

    // Density of the cell the sampler actually drew (from the CDF), not a re-derived luminance.
    pdfSolidAngle = EnvNeeCellPdf(cellIndex);
    return pdfSolidAngle > 0.0;
}

#endif
