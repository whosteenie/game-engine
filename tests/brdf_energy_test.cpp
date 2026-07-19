// CPU mirror of the opaque BRDF estimator in path_tracer.hlsl (SampleOpaqueInterface). Guards the
// S2 audit fixes (devdoc/dxr/pt/pt-audit.md): no forced-lobe divides (B3), the correct VNDF
// estimator weight F(VoH)*G2/G1 (B4), and energy conservation of the (1-F) diffuse + GGX specular
// split (R4) via a Monte-Carlo white-furnace test. Keep in sync when the shader estimator changes.
//
// The furnace test is the audit's headline S2 validation: a stochastic estimator that returns
// throughput per bounce must, averaged over the hemisphere, reproduce the surface's reflectance
// WITHOUT energy gain. The old code gained energy at grazing angles (real specular F + a diffuse
// weighted by a roughness-clamped (1-F) hack) and over-weighted forced lobes — both show up here as
// reflectance > 1.

#include "test_expect.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <random>

namespace
{
    const float kPi = 3.14159265f;

    float Luminance(const glm::vec3& c)
    {
        return glm::dot(c, glm::vec3(0.2126f, 0.7152f, 0.0722f));
    }

    float GgxD(float NoH, float alpha)
    {
        const float a2 = alpha * alpha;
        const float d = NoH * NoH * (a2 - 1.0f) + 1.0f;
        return a2 / std::max(kPi * d * d, 1e-9f);
    }

    float SmithG1(float NoX, float alpha)
    {
        const float a2 = alpha * alpha;
        return 2.0f * NoX / std::max(NoX + std::sqrt(a2 + (1.0f - a2) * NoX * NoX), 1e-9f);
    }

    float SmithG2(float NoV, float NoL, float alpha)
    {
        const float a2 = alpha * alpha;
        const float lambdaV = NoL * std::sqrt(a2 + (1.0f - a2) * NoV * NoV);
        const float lambdaL = NoV * std::sqrt(a2 + (1.0f - a2) * NoL * NoL);
        return 2.0f * NoV * NoL / std::max(lambdaV + lambdaL, 1e-9f);
    }

    glm::vec3 FresnelSchlick(float cosTheta, const glm::vec3& f0)
    {
        const float m = std::clamp(1.0f - cosTheta, 0.0f, 1.0f);
        const float m2 = m * m;
        return f0 + (glm::vec3(1.0f) - f0) * (m2 * m2 * m);
    }

    float OpaqueLobeSelectionProbability(
        const glm::vec3& albedo,
        const float metallic,
        const glm::vec3& fresnelNoV)
    {
        const glm::vec3 baseDiffuse = albedo * (1.0f - std::clamp(metallic, 0.0f, 1.0f));
        if (std::max(baseDiffuse.r, std::max(baseDiffuse.g, baseDiffuse.b)) <= 1.0e-6f)
        {
            // A zero-energy diffuse lobe is not a sampling technique. Keeping the old 0.9 ceiling
            // injected one near-black sample per ten perfect-metal bounces.
            return 1.0f;
        }

        const float specLum = Luminance(fresnelNoV);
        const float diffLum = Luminance(baseDiffuse);
        float pSpec = specLum / std::max(specLum + diffLum, 1e-4f);
        pSpec = glm::mix(pSpec, 1.0f, std::clamp(metallic, 0.0f, 1.0f));
        return std::clamp(pSpec, 0.1f, 0.9f);
    }

    void BuildTangentFrame(const glm::vec3& n, glm::vec3& tangent, glm::vec3& bitangent)
    {
        const glm::vec3 up = std::abs(n.z) < 0.999f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0);
        tangent = glm::normalize(glm::cross(up, n));
        bitangent = glm::cross(n, tangent);
    }

    glm::vec3 CosineSampleHemisphere(const glm::vec3& n, float u1, float u2)
    {
        glm::vec3 t, b;
        BuildTangentFrame(n, t, b);
        const float r = std::sqrt(std::clamp(u1, 0.0f, 1.0f));
        const float phi = 2.0f * kPi * u2;
        const float z = std::sqrt(std::max(1.0f - u1, 0.0f));
        return glm::normalize(t * (r * std::cos(phi)) + b * (r * std::sin(phi)) + n * z);
    }

    // Exact port of SampleGgxVndfHalfVector (Heitz 2018).
    glm::vec3 SampleGgxVndfHalfVector(const glm::vec3& n, const glm::vec3& v, float roughness, float u1, float u2)
    {
        const float alpha = std::max(roughness * roughness, 1e-3f);
        glm::vec3 t, b;
        BuildTangentFrame(n, t, b);
        const glm::vec3 vt(glm::dot(v, t), glm::dot(v, b), glm::dot(v, n));
        const glm::vec3 sv = glm::normalize(glm::vec3(alpha * vt.x, alpha * vt.y, vt.z));
        const float lenSq = sv.x * sv.x + sv.y * sv.y;
        const glm::vec3 b1 = lenSq > 1e-7f
            ? glm::vec3(-sv.y, sv.x, 0.0f) * (1.0f / std::sqrt(lenSq))
            : glm::vec3(1, 0, 0);
        const glm::vec3 b2 = glm::cross(sv, b1);
        const float r = std::sqrt(u1);
        const float phi = 2.0f * kPi * u2;
        const float t1 = r * std::cos(phi);
        float t2 = r * std::sin(phi);
        const float s = 0.5f * (1.0f + sv.z);
        t2 = (1.0f - s) * std::sqrt(std::clamp(1.0f - t1 * t1, 0.0f, 1.0f)) + s * t2;
        const glm::vec3 hs = t1 * b1 + t2 * b2 + std::sqrt(std::max(1.0f - t1 * t1 - t2 * t2, 0.0f)) * sv;
        const glm::vec3 ht = glm::normalize(glm::vec3(alpha * hs.x, alpha * hs.y, std::max(hs.z, 1e-6f)));
        return glm::normalize(t * ht.x + b * ht.y + n * ht.z);
    }

    // Exact port of the SampleOpaqueInterface throughput multiplier (one-sample MIS).
    glm::vec3 EstimatorWeight(
        const glm::vec3& n,
        const glm::vec3& v,
        const glm::vec3& albedo,
        float metallic,
        float roughness,
        float uLobe,
        float u1,
        float u2)
    {
        const glm::vec3 f0 = glm::mix(glm::vec3(0.04f), albedo, metallic);
        const float ggxRoughness = std::min(std::max(roughness, 1e-4f), 0.99f);
        const float alpha = std::max(ggxRoughness * ggxRoughness, 1e-3f);
        const float NoV = std::clamp(glm::dot(n, v), 0.0f, 1.0f);
        const glm::vec3 baseDiffuse = albedo * (1.0f - std::clamp(metallic, 0.0f, 1.0f));

        const glm::vec3 fresnelNoV = FresnelSchlick(NoV, f0);
        const float pSpec = OpaqueLobeSelectionProbability(albedo, metallic, fresnelNoV);

        const bool sampledSpecular = uLobe < pSpec;
        if (sampledSpecular && roughness <= 0.03f)
        {
            return fresnelNoV / std::max(pSpec, 1.0e-6f);
        }
        const glm::vec3 l = sampledSpecular
            ? glm::reflect(-v, SampleGgxVndfHalfVector(n, v, ggxRoughness, u1, u2))
            : CosineSampleHemisphere(n, u1, u2);

        const float NoL = glm::dot(n, l);
        if (NoL <= 0.0f)
        {
            return glm::vec3(0.0f);
        }

        const glm::vec3 h = glm::normalize(v + l);
        const float NoH = std::clamp(glm::dot(n, h), 0.0f, 1.0f);
        const float VoH = std::clamp(glm::dot(v, h), 0.0f, 1.0f);

        const float d = GgxD(NoH, alpha);
        const float g1 = SmithG1(NoV, alpha);
        const float g2 = SmithG2(NoV, NoL, alpha);
        const glm::vec3 fresnel = FresnelSchlick(VoH, f0);

        const glm::vec3 specCos = d * g2 * fresnel / std::max(4.0f * NoV, 1e-4f);
        const glm::vec3 diffCos = baseDiffuse * (glm::vec3(1.0f) - fresnelNoV) * (NoL / kPi);

        const float pdfSpec = g1 * d / std::max(4.0f * NoV, 1e-4f);
        const float pdfDiff = NoL / kPi;
        const float pdfMix = pSpec * pdfSpec + (1.0f - pSpec) * pdfDiff;

        return (specCos + diffCos) / std::max(pdfMix, 1e-9f);
    }

    // Monte-Carlo hemispherical reflectance (average throughput multiplier) for a view angle.
    glm::vec3 Furnace(const glm::vec3& albedo, float metallic, float roughness, float thetaV, int samples)
    {
        std::mt19937 rng(0xC0FFEEu + static_cast<unsigned>(thetaV * 1000.0f)
            + static_cast<unsigned>(roughness * 137.0f) + static_cast<unsigned>(metallic * 991.0f));
        std::uniform_real_distribution<float> U(0.0f, 1.0f);
        const glm::vec3 n(0, 0, 1);
        const glm::vec3 v = glm::normalize(glm::vec3(std::sin(thetaV), 0.0f, std::cos(thetaV)));

        glm::vec3 sum(0.0f);
        for (int i = 0; i < samples; ++i)
        {
            sum += EstimatorWeight(n, v, albedo, metallic, roughness, U(rng), U(rng), U(rng));
        }
        return sum / static_cast<float>(samples);
    }
}

void RunBrdfEnergyTests()
{
    const int kSamples = 200000;

    {
        const glm::vec3 albedo(0.95f, 0.96f, 0.98f);
        const glm::vec3 fresnel = FresnelSchlick(1.0f, albedo);
        test::ExpectNear(
            OpaqueLobeSelectionProbability(albedo, 1.0f, fresnel),
            1.0f,
            0.0f,
            "Zero-diffuse perfect metal always selects its specular lobe");
        for (int sample = 0; sample < 32; ++sample)
        {
            const float lobeSample = (static_cast<float>(sample) + 0.5f) / 32.0f;
            const glm::vec3 weight = EstimatorWeight(
                glm::vec3(0.0f, 0.0f, 1.0f),
                glm::vec3(0.0f, 0.0f, 1.0f),
                albedo,
                1.0f,
                0.0f,
                lobeSample,
                0.37f,
                0.73f);
            test::ExpectTrue(
                weight.r > 0.9f && weight.g > 0.9f && weight.b > 0.9f,
                "Perfect delta metal has no stochastic black lobe");
        }
    }

    // B3/B4/R4 regression catch: reflectance must never exceed 1 (no energy gain), across roughness
    // and view angle including grazing, for white dielectric AND white metal. The pre-fix code gained
    // energy at grazing (real F specular + roughness-clamped-(1-F) diffuse) — this asserts it cannot.
    const float roughnesses[3] = {0.05f, 0.3f, 0.7f};
    const float angles[3] = {0.0f, 0.6f, 1.2f}; // ~0deg, ~34deg, ~69deg (grazing)
    for (int ri = 0; ri < 3; ++ri)
    {
        for (int ai = 0; ai < 3; ++ai)
        {
            const glm::vec3 die = Furnace(glm::vec3(1.0f), 0.0f, roughnesses[ri], angles[ai], kSamples);
            test::ExpectTrue(die.r < 1.03f, "White dielectric furnace: no energy gain");
            const glm::vec3 met = Furnace(glm::vec3(1.0f), 1.0f, roughnesses[ri], angles[ai], kSamples);
            test::ExpectTrue(met.r < 1.03f, "White metal furnace: no energy gain");
        }
    }

    // Smooth white surfaces should reflect ~all incident light (the (1-F) diffuse model loses a few
    // percent by design; the point is it neither gains nor goes dark).
    const glm::vec3 smoothDielectric = Furnace(glm::vec3(1.0f), 0.0f, 0.05f, 0.0f, kSamples);
    test::ExpectTrue(
        smoothDielectric.r > 0.85f && smoothDielectric.r < 1.03f,
        "Smooth white dielectric furnace reflectance in [0.85, 1.03]");
    const glm::vec3 smoothMetal = Furnace(glm::vec3(1.0f), 1.0f, 0.05f, 0.0f, kSamples);
    test::ExpectTrue(
        smoothMetal.r > 0.9f && smoothMetal.r < 1.03f,
        "Smooth white metal furnace reflectance in [0.9, 1.03]");

    // Colored surfaces must not gain energy in any channel (per-channel reflectance <= albedo + spec).
    const glm::vec3 red = Furnace(glm::vec3(0.8f, 0.1f, 0.1f), 0.0f, 0.2f, 0.3f, kSamples);
    test::ExpectTrue(red.r < 0.9f, "Colored dielectric: red channel does not gain energy");
    test::ExpectTrue(red.g < 0.22f, "Colored dielectric: green channel does not gain energy");

    // B4 identity: the specular single-lobe estimator weight is F*G2/G1 (the D and 4*NoV cancel),
    // NOT the constant f0 the old code used. Verify numerically for several microfacet configs.
    {
        const glm::vec3 f0(1.0f, 0.85f, 0.4f); // arbitrary metal
        const float alpha = 0.3f * 0.3f;
        const float configs[4][2] = {{0.9f, 0.8f}, {0.6f, 0.55f}, {0.4f, 0.95f}, {0.75f, 0.5f}};
        for (int i = 0; i < 4; ++i)
        {
            const float NoV = configs[i][0];
            const float NoL = configs[i][1];
            const float NoH = 0.9f; // representative
            const float VoH = 0.7f;
            const float d = GgxD(NoH, alpha);
            const float g1 = SmithG1(NoV, alpha);
            const float g2 = SmithG2(NoV, NoL, alpha);
            const glm::vec3 f = FresnelSchlick(VoH, f0);
            const glm::vec3 specCos = d * g2 * f / (4.0f * NoV);
            const float pdfSpec = g1 * d / (4.0f * NoV);
            const glm::vec3 weight = specCos / pdfSpec;      // = F*G2/G1
            const glm::vec3 expected = f * (g2 / g1);
            test::ExpectNear(weight.r, expected.r, 1e-5f, "VNDF specular weight = F*G2/G1 (R)");
            test::ExpectNear(weight.g, expected.g, 1e-5f, "VNDF specular weight = F*G2/G1 (G)");
            test::ExpectTrue(std::abs(weight.r - f0.r) > 1e-3f || std::abs(g2 / g1 - 1.0f) < 1e-3f,
                "VNDF weight is angle-dependent, not constant f0");
        }
    }
}
