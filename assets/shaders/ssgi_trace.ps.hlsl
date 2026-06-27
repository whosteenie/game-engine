// SSGI Phase 6 — view-space screen trace with depth buffer ray march.
// One cosine-weighted hemisphere ray per pixel; hit samples radiance assembly buffer.
// Miss returns 0 (incremental — IBL remains in composite).

Texture2D uDepthMap : register(t0);
Texture2D uNormalMap : register(t1);
Texture2D uMaterial0Map : register(t2);
Texture2D uMaterial1Map : register(t3);
Texture2D uRadianceMap : register(t4);

SamplerState uDepthSampler : register(s0);
SamplerState uNormalSampler : register(s1);
SamplerState uMaterial0Sampler : register(s2);
SamplerState uMaterial1Sampler : register(s3);
SamplerState uRadianceSampler : register(s4);

cbuffer PerPixel : register(b0)
{
    float4x4 uInvProjection;
    float4x4 uProjection;
    float4x4 uView;
    float uMaxTraceDistance;
    int uStepCount;
    float uThickness;
    float uFrameIndex;
    float uEdgeFadeScale;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

static const float kPi = 3.14159265;

float Hash21(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 34.345);
    return frac(p.x * p.y);
}

float2 DepthUvToClipXY(float2 texCoord)
{
    return float2(texCoord.x * 2.0 - 1.0, 1.0 - texCoord.y * 2.0);
}

float3 ViewPosFromDepth(float2 texCoord, float depth)
{
    const float2 clipXY = DepthUvToClipXY(texCoord);
    float4 viewH = mul(uInvProjection, float4(clipXY, depth, 1.0));
    return viewH.xyz / viewH.w;
}

float2 ViewPosToDepthUv(float3 viewPos)
{
    float4 clipH = mul(uProjection, float4(viewPos, 1.0));
    clipH.xyz /= clipH.w;
    return float2(clipH.x * 0.5 + 0.5, (1.0 - clipH.y) * 0.5);
}

void BuildTangentFrame(float3 normal, out float3 tangent, out float3 bitangent)
{
    const float3 up = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    tangent = normalize(cross(up, normal));
    bitangent = cross(normal, tangent);
}

float3 CosineHemisphereDirection(float2 xi, float3 normal)
{
    float3 tangent;
    float3 bitangent;
    BuildTangentFrame(normal, tangent, bitangent);

    const float cosTheta = sqrt(saturate(xi.x));
    const float sinTheta = sqrt(saturate(1.0 - xi.x));
    const float phi = kPi * 2.0 * xi.y;
    const float3 localDir = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    return normalize(localDir.x * tangent + localDir.y * bitangent + localDir.z * normal);
}

float EdgeFade(float2 uv, float scale)
{
    const float2 edgeDist = min(uv, 1.0 - uv);
    return saturate(min(edgeDist.x, edgeDist.y) * scale);
}

float DiffuseSsgiWeight(float roughness, float metallic)
{
    return (1.0 - metallic) * lerp(0.35, 1.0, roughness);
}

float4 main(PSInput input) : SV_Target
{
    const float2 uv = input.texCoord;
    const float depth = uDepthMap.Sample(uDepthSampler, uv).r;
    if (depth >= 0.9999)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    const float4 material0 = uMaterial0Map.Sample(uMaterial0Sampler, uv);
    const float roughness = material0.a;
    const float metallic = uMaterial1Map.Sample(uMaterial1Sampler, uv).r;
    const float diffuseWeight = DiffuseSsgiWeight(roughness, metallic);
    if (diffuseWeight <= 1e-4)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    const float3 worldNormal = normalize(uNormalMap.Sample(uNormalSampler, uv).rgb);
    float3 viewNormal = mul((float3x3)uView, worldNormal);
    viewNormal = normalize(viewNormal);

    const float3 viewPos = ViewPosFromDepth(uv, depth);
    const float2 xi = float2(
        Hash21(uv + float2(uFrameIndex * 0.013, 0.0)),
        Hash21(uv.yx + float2(0.0, uFrameIndex * 0.027)));
    const float3 rayDir = CosineHemisphereDirection(xi, viewNormal);

    const int stepCount = max(uStepCount, 1);
    const float stepSize = uMaxTraceDistance / (float)stepCount;

    float3 hitRadiance = 0.0.xxx;
    float hitDistance01 = 0.0;
    float3 marchPos = viewPos;

    [loop]
    for (int stepIndex = 0; stepIndex < stepCount; ++stepIndex)
    {
        marchPos += rayDir * stepSize;
        const float traveled = length(marchPos - viewPos);
        if (traveled > uMaxTraceDistance)
        {
            break;
        }

        const float2 sampleUv = ViewPosToDepthUv(marchPos);
        if (sampleUv.x < 0.0 || sampleUv.x > 1.0 || sampleUv.y < 0.0 || sampleUv.y > 1.0)
        {
            break;
        }

        const float sampleDepth = uDepthMap.Sample(uDepthSampler, sampleUv).r;
        if (sampleDepth >= 0.9999)
        {
            continue;
        }

        const float3 sampleViewPos = ViewPosFromDepth(sampleUv, sampleDepth);
        const float depthDelta = marchPos.z - sampleViewPos.z;
        if (depthDelta > 0.0 && depthDelta < uThickness)
        {
            const float4 radiance = uRadianceMap.Sample(uRadianceSampler, sampleUv);
            if (radiance.a > 0.5)
            {
                hitRadiance = radiance.rgb;
                hitDistance01 = saturate(traveled / max(uMaxTraceDistance, 1e-4));
            }
            break;
        }
    }

    const float edgeFade = EdgeFade(uv, uEdgeFadeScale);
    return float4(hitRadiance * diffuseWeight * edgeFade, hitDistance01);
}
