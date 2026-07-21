// Integration-level guards for the S3–S5 NEE/MIS estimators (devdoc/dxr/pt/pt-audit.md §0b).
// The pre-existing mis_nee / env_importance tests only exercised helpers in isolation; C1–C4 were all
// "the two halves of an estimator disagree" bugs that only surface when a strategy is integrated
// end-to-end. These tests integrate each estimator and assert the invariants those bugs violated:
//   C1 — Fresnel-proportional dielectric selection must be throughput-preserving (mean weight = 1).
//   C3 — emissive-NEE area sampling must agree with BSDF sampling of the same area light.
//   C4 — env-IS pdf must be intensity-invariant and integrate to 1 over the sphere.

#include "engine/lighting/environment/Importance.h"
#include "test_expect.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace
{
    const float kPi = glm::pi<float>();

    // ---- C1: reference-mode stochastic Fresnel split, ported from SampleDielectricInterface -------
    float FresnelDielectric(float cosThetaI, float eta)
    {
        const float cosI = std::abs(cosThetaI);
        const float sin2T = eta * eta * (1.0f - cosI * cosI);
        if (sin2T >= 1.0f)
        {
            return 1.0f;
        }
        const float cosT = std::sqrt(std::max(1.0f - sin2T, 0.0f));
        const float rPar = (eta * cosI - cosT) / std::max(eta * cosI + cosT, 1e-6f);
        const float rPerp = (cosI - eta * cosT) / std::max(cosI + eta * cosT, 1e-6f);
        return std::clamp(0.5f * (rPar * rPar + rPerp * rPerp), 0.0f, 1.0f);
    }

    // Mean throughput multiplier through a lossless glass interface under Fresnel-proportional
    // selection. Post-fix each branch weight is R/P = 1, so the mean is 1 (energy preserved). The
    // pre-fix code divided by F / (1-F), giving mean F*(1/F) + (1-F)*(1/(1-F)) = 2 — a 2x energy gain
    // (and up to 1/F ≈ 25x on any individual near-normal reflection).
    float StochasticDielectricMeanThroughput(float cosThetaI, float eta, bool buggy, int samples)
    {
        std::mt19937 rng(777);
        std::uniform_real_distribution<float> U(0.0f, 1.0f);
        const float fresnel = FresnelDielectric(cosThetaI, eta);
        double sum = 0.0;
        for (int i = 0; i < samples; ++i)
        {
            const bool chooseReflect = U(rng) < fresnel;
            const float weight = chooseReflect
                ? (buggy ? 1.0f / std::max(fresnel, 1e-6f) : 1.0f)
                : (buggy ? 1.0f / std::max(1.0f - fresnel, 1e-6f) : 1.0f);
            sum += weight;
        }
        return static_cast<float>(sum / samples);
    }

    // ---- C3: area light + Lambertian receiver ------------------------------------------------------
    struct Triangle
    {
        glm::vec3 v0, v1, v2, faceNormal;
        float area;
    };

    Triangle MakeTriangle(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
    {
        Triangle t{a, b, c, glm::vec3(0), 0.0f};
        const glm::vec3 cr = glm::cross(b - a, c - a);
        const float len = glm::length(cr);
        t.area = 0.5f * len;
        t.faceNormal = len > 1e-8f ? cr / len : glm::vec3(0, 0, 1);
        return t;
    }

    // Möller–Trumbore; returns hit distance along dir (or -1 on miss). Single-sided against faceNormal
    // is not required — we accept either winding since the receiver is on the emitting side.
    float RayTriangle(const glm::vec3& o, const glm::vec3& dir, const Triangle& t)
    {
        const glm::vec3 e1 = t.v1 - t.v0;
        const glm::vec3 e2 = t.v2 - t.v0;
        const glm::vec3 p = glm::cross(dir, e2);
        const float det = glm::dot(e1, p);
        if (std::abs(det) < 1e-9f)
        {
            return -1.0f;
        }
        const float inv = 1.0f / det;
        const glm::vec3 s = o - t.v0;
        const float u = glm::dot(s, p) * inv;
        if (u < 0.0f || u > 1.0f)
        {
            return -1.0f;
        }
        const glm::vec3 q = glm::cross(s, e1);
        const float v = glm::dot(dir, q) * inv;
        if (v < 0.0f || u + v > 1.0f)
        {
            return -1.0f;
        }
        const float dist = glm::dot(e2, q) * inv;
        return dist > 1e-5f ? dist : -1.0f;
    }

    glm::vec3 CosineSampleHemisphere(const glm::vec3& n, float u1, float u2)
    {
        const glm::vec3 up = std::abs(n.z) < 0.999f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0);
        const glm::vec3 t = glm::normalize(glm::cross(up, n));
        const glm::vec3 b = glm::cross(n, t);
        const float r = std::sqrt(std::clamp(u1, 0.0f, 1.0f));
        const float phi = 2.0f * kPi * u2;
        const float z = std::sqrt(std::max(1.0f - u1, 0.0f));
        return glm::normalize(t * (r * std::cos(phi)) + b * (r * std::sin(phi)) + n * z);
    }

    // NEE (area sampling) estimate of direct lighting with the CORRECTED geometry term (cosE/dist²
    // only — EvaluateOpaqueBsdf already carries cosThetaReceiver).
    float EmissiveNeeEstimate(
        const glm::vec3& x, const glm::vec3& n, float albedo, const Triangle& light, float Le, int samples)
    {
        std::mt19937 rng(1234);
        std::uniform_real_distribution<float> U(0.0f, 1.0f);
        const float f = albedo / kPi;
        const float pdfArea = 1.0f / light.area;
        double sum = 0.0;
        for (int i = 0; i < samples; ++i)
        {
            const float su0 = std::sqrt(U(rng));
            const float bu = 1.0f - su0;
            const float bv = U(rng) * su0;
            const glm::vec3 p = bu * light.v0 + bv * light.v1 + (1.0f - bu - bv) * light.v2;
            const glm::vec3 to = p - x;
            const float dist2 = std::max(glm::dot(to, to), 1e-8f);
            const glm::vec3 wi = to / std::sqrt(dist2);
            const float cosR = std::max(glm::dot(n, wi), 0.0f);
            const float cosE = std::max(glm::dot(light.faceNormal, -wi), 0.0f);
            if (cosR <= 0.0f || cosE <= 0.0f)
            {
                continue;
            }
            const float bsdfTimesNoL = f * cosR;            // EvaluateOpaqueBsdf returns f * cosR
            const float geometry = cosE / dist2;             // CORRECTED (no extra cosR)
            sum += bsdfTimesNoL * Le * geometry / pdfArea;
        }
        return static_cast<float>(sum / samples);
    }

    // BSDF (cosine) sampling estimate of the same integral. For a Lambertian this reduces to
    // albedo*Le whenever the sampled ray hits the light — an independent estimator of the same value.
    float BsdfHitEstimate(
        const glm::vec3& x, const glm::vec3& n, float albedo, const Triangle& light, float Le, int samples)
    {
        std::mt19937 rng(4321);
        std::uniform_real_distribution<float> U(0.0f, 1.0f);
        double sum = 0.0;
        for (int i = 0; i < samples; ++i)
        {
            const glm::vec3 wi = CosineSampleHemisphere(n, U(rng), U(rng));
            if (RayTriangle(x, wi, light) > 0.0f)
            {
                // f*cosR/pdf = (albedo/pi * cosR)/(cosR/pi) = albedo.
                sum += albedo * Le;
            }
        }
        return static_cast<float>(sum / samples);
    }

    // ---- C4: env-IS pdf, mirrors EnvNeeCellPdf -----------------------------------------------------
    float CosLat(int iy, int h)
    {
        const float v = (static_cast<float>(iy) + 0.5f) / static_cast<float>(h);
        return std::max(std::cos(kPi * (v - 0.5f)), 1e-6f);
    }

    float CellSolidAngle(int iy, int w, int h)
    {
        return (2.0f * kPi / static_cast<float>(w)) * (kPi / static_cast<float>(h)) * CosLat(iy, h);
    }
}

void RunNeeMisIntegrationTests()
{
    // C1: lossless interface preserves energy under Fresnel-proportional selection.
    {
        const float m0 = StochasticDielectricMeanThroughput(0.9f, 1.0f / 1.5f, false, 200000);
        const float m1 = StochasticDielectricMeanThroughput(0.2f, 1.0f / 1.5f, false, 200000);
        test::ExpectNear(m0, 1.0f, 1e-3f, "Dielectric split preserves energy at near-normal incidence");
        test::ExpectNear(m1, 1.0f, 1e-3f, "Dielectric split preserves energy at grazing incidence");
        // Sanity: the pre-fix divisions doubled mean throughput — confirm the guard detects it.
        // (High variance: the reflect weight is 1/F ≈ 25 at 4% frequency, so use a loose band.)
        const float buggy = StochasticDielectricMeanThroughput(0.9f, 1.0f / 1.5f, true, 200000);
        test::ExpectTrue(buggy > 1.5f, "Pre-fix divide-by-Fresnel inflates energy well above 1 (guard check)");
    }

    // C3: emissive NEE (corrected geometry) agrees with BSDF sampling of the same area light.
    {
        const glm::vec3 x(0, 0, 0);
        const glm::vec3 n(0, 0, 1);
        const float albedo = 0.8f;
        const float Le = 3.0f;
        // Triangle above the receiver, wound so its face normal points down toward it.
        const Triangle light = MakeTriangle(
            glm::vec3(-0.6f, -0.6f, 2.0f), glm::vec3(0.0f, 0.7f, 2.0f), glm::vec3(0.6f, -0.6f, 2.0f));
        test::ExpectTrue(light.faceNormal.z < 0.0f, "Test light faces the receiver");

        const float nee = EmissiveNeeEstimate(x, n, albedo, light, Le, 400000);
        const float bsdf = BsdfHitEstimate(x, n, albedo, light, Le, 400000);
        test::ExpectTrue(nee > 1e-4f, "Emissive NEE produces nonzero direct lighting");
        // Both are unbiased estimators of the same integral; agree within MC noise. A doubled receiver
        // cosine (the C3 bug) would make NEE ~30-40% smaller than BSDF here.
        test::ExpectNear(nee / bsdf, 1.0f, 0.03f, "Emissive NEE agrees with BSDF sampling (single cos)");
    }

    // C4a: env-IS cell pdf is invariant to a global radiance (intensity) scale.
    {
        const int w = 8, h = 4;
        std::vector<float> hdr(static_cast<std::size_t>(w * h * 4), 0.0f);
        // A couple of bright cells so the CDF is non-uniform.
        auto setCell = [&](int x, int y, float value) {
            const std::size_t idx = (static_cast<std::size_t>(y) * w + x) * 4;
            hdr[idx + 0] = value;
            hdr[idx + 1] = value;
            hdr[idx + 2] = value;
        };
        for (std::size_t i = 0; i < hdr.size(); i += 4)
        {
            hdr[i] = hdr[i + 1] = hdr[i + 2] = 0.2f;
        }
        setCell(5, 2, 20.0f);
        setCell(1, 1, 6.0f);

        std::vector<float> hdr10 = hdr;
        for (float& c : hdr10)
        {
            c *= 10.0f;
        }

        const EnvImportanceSamplingBuildResult a = BuildEquirectEnvImportanceCdf(hdr, w, h, 64, 64);
        const EnvImportanceSamplingBuildResult b = BuildEquirectEnvImportanceCdf(hdr10, w, h, 64, 64);
        test::ExpectTrue(!a.cdf.empty() && a.cdfWidth == w && a.cdfHeight == h, "Env CDF built");
        test::ExpectTrue(a.cdf.size() == b.cdf.size(), "Env CDF sizes match under intensity scale");

        float maxProbDelta = 0.0f;
        double integral = 0.0;
        for (std::size_t cell = 0; cell + 1 < a.cdf.size(); ++cell)
        {
            const float probA = a.cdf[cell + 1] - a.cdf[cell];
            const float probB = b.cdf[cell + 1] - b.cdf[cell];
            maxProbDelta = std::max(maxProbDelta, std::abs(probA - probB));
            const int iy = static_cast<int>(cell) / a.cdfWidth;
            const float omega = CellSolidAngle(iy, a.cdfWidth, a.cdfHeight);
            const float pdf = probA / std::max(omega, 1e-8f);
            integral += static_cast<double>(pdf) * omega; // == Σ probA
        }
        test::ExpectNear(maxProbDelta, 0.0f, 1e-6f, "Env-IS cell pdf is intensity-invariant (C4)");
        // C4b: the pdf integrates to 1 over the sphere (it is a normalized density).
        test::ExpectNear(static_cast<float>(integral), 1.0f, 1e-4f, "Env-IS pdf integrates to 1");
    }
}
