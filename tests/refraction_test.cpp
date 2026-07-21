// CPU mirror of assets/shaders/raytracing/path_tracing/pt_dielectric.hlsli.
// Keep in sync when the shader formulas change. These guard the S1 audit fixes (devdoc/dxr/pt/
// pt-audit.md): RefractSnell's tangential sign (B1) and RefractThinSlab straight-through (B1b),
// plus the two-face slab Fresnel remap (A3). A revert of the sign bug fails RefractSnellMatchesTruth.

#include "test_expect.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace
{
    // Exact CPU mirror of the FIXED pt_dielectric.hlsli RefractSnell. wi points away from the surface
    // along the incoming path; returns the transmitted PROPAGATION direction.
    bool RefractSnell(glm::vec3 wi, glm::vec3 n, float eta, glm::vec3& wt)
    {
        float cosI = glm::dot(wi, n);
        if (cosI < 0.0f)
        {
            n = -n;
            cosI = -cosI;
        }

        const float sin2T = eta * eta * (1.0f - cosI * cosI);
        if (sin2T > 1.0f)
        {
            wt = glm::vec3(0.0f);
            return false;
        }

        const float cosT = std::sqrt(std::max(1.0f - sin2T, 0.0f));
        wt = -eta * wi + (eta * cosI - cosT) * n;
        return glm::dot(wt, wt) > 1e-8f;
    }

    // Physical ground truth (GLSL refract convention). d = incident propagation direction; n opposes d.
    bool RefractTruth(glm::vec3 d, glm::vec3 n, float eta, glm::vec3& out)
    {
        d = glm::normalize(d);
        const float cosI = -glm::dot(d, n);
        const float sin2T = eta * eta * (1.0f - cosI * cosI);
        if (sin2T > 1.0f)
        {
            return false;
        }
        const float cosT = std::sqrt(1.0f - sin2T);
        out = glm::normalize(eta * d + (eta * cosI - cosT) * n);
        return true;
    }

    // Exact CPU mirror of the FIXED RefractThinSlab: a zero-thickness slab transmits straight through.
    bool RefractThinSlab(glm::vec3 wi, glm::vec3 /*n*/, float /*ior*/, glm::vec3& wo)
    {
        wo = -wi;
        return true;
    }

    // Two-face slab reflectance incl. internal bounces (PBRT ThinDielectric): R_slab = 2R/(1+R).
    float SlabReflectance(float singleFaceR)
    {
        return 2.0f * singleFaceR / (1.0f + singleFaceR);
    }

    // Build a unit incident propagation direction at angle theta (radians) from -n, in the n/tangent
    // plane. Returns d (into the surface) with n on the opposite side (dot(d, n) < 0).
    glm::vec3 IncidentDir(const glm::vec3& n, const glm::vec3& tangent, float theta)
    {
        return glm::normalize(-std::cos(theta) * n + std::sin(theta) * tangent);
    }
}

void RunRefractionTests()
{
    const glm::vec3 n(0.0f, 0.0f, 1.0f);
    const glm::vec3 tangent(1.0f, 0.0f, 0.0f);

    // B1 regression guard: known 45deg air->glass case. The OLD (buggy) formula gave x = -0.4714.
    {
        const glm::vec3 d = IncidentDir(n, tangent, glm::radians(45.0f));
        glm::vec3 wt{};
        const bool ok = RefractSnell(-d, n, 1.0f / 1.5f, wt);
        wt = glm::normalize(wt);
        test::ExpectTrue(ok, "45deg air->glass transmits");
        test::ExpectNear(wt.x, 0.4714045f, 1e-5f, "Refract 45deg tangential x (sign + magnitude)");
        test::ExpectNear(wt.z, -0.8819171f, 1e-5f, "Refract 45deg normal z");
        test::ExpectTrue(wt.x > 0.0f, "Refracted ray stays on the incident tangential side (no mirror)");
    }

    // RefractSnell matches physical truth across angles and IORs, entering (eta = 1/ior).
    {
        float worstError = 0.0f;
        for (int iorStep = 0; iorStep <= 6; ++iorStep)
        {
            const float ior = 1.05f + 0.25f * static_cast<float>(iorStep);
            for (int angleStep = 0; angleStep < 89; ++angleStep)
            {
                const float theta = glm::radians(static_cast<float>(angleStep));
                const glm::vec3 d = IncidentDir(n, tangent, theta);
                glm::vec3 wt{};
                glm::vec3 truth{};
                const bool ok = RefractSnell(-d, n, 1.0f / ior, wt);
                const bool okTruth = RefractTruth(d, n, 1.0f / ior, truth);
                test::ExpectTrue(ok == okTruth, "Entering transmit/TIR agrees with truth");
                if (ok && okTruth)
                {
                    worstError = std::max(worstError, glm::length(glm::normalize(wt) - truth));
                }
            }
        }
        test::ExpectNear(worstError, 0.0f, 1e-5f, "RefractSnell matches physical refraction (entering)");
    }

    // Exiting (eta = ior) must detect total internal reflection exactly where truth does.
    {
        int tirCases = 0;
        for (int iorStep = 0; iorStep <= 6; ++iorStep)
        {
            const float ior = 1.05f + 0.25f * static_cast<float>(iorStep);
            for (int angleStep = 0; angleStep < 89; ++angleStep)
            {
                const float theta = glm::radians(static_cast<float>(angleStep));
                const glm::vec3 d = IncidentDir(n, tangent, theta);
                glm::vec3 wt{};
                glm::vec3 truth{};
                const bool ok = RefractSnell(-d, n, ior, wt);
                const bool okTruth = RefractTruth(d, n, ior, truth);
                test::ExpectTrue(ok == okTruth, "Exiting transmit/TIR agrees with truth");
                if (!okTruth)
                {
                    ++tirCases;
                }
                else if (ok)
                {
                    test::ExpectNear(
                        glm::length(glm::normalize(wt) - truth),
                        0.0f,
                        1e-5f,
                        "RefractSnell matches physical refraction (exiting)");
                }
            }
        }
        test::ExpectTrue(tirCases > 0, "Exiting sweep exercises TIR");
    }

    // B1b: thin slab transmits straight through, regardless of angle/IOR.
    {
        float worstDeviationDeg = 0.0f;
        for (int angleStep = 0; angleStep < 89; ++angleStep)
        {
            const float theta = glm::radians(static_cast<float>(angleStep));
            const glm::vec3 d = IncidentDir(n, tangent, theta);
            glm::vec3 wo{};
            const bool ok = RefractThinSlab(-d, n, 1.5f, wo);
            test::ExpectTrue(ok, "Thin slab always transmits");
            const float deviation =
                glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(wo), d), -1.0f, 1.0f)));
            worstDeviationDeg = std::max(worstDeviationDeg, deviation);
        }
        test::ExpectNear(worstDeviationDeg, 0.0f, 1e-3f, "Thin slab is straight-through (no deviation)");
    }

    // A3: two-face slab reflectance remap.
    {
        test::ExpectNear(SlabReflectance(0.0f), 0.0f, 1e-6f, "Slab R at R=0");
        test::ExpectNear(SlabReflectance(0.04f), 0.0769231f, 1e-5f, "Slab R at R=0.04 (glass)");
        test::ExpectNear(SlabReflectance(1.0f), 1.0f, 1e-6f, "Slab R at R=1 (TIR)");
        test::ExpectTrue(
            SlabReflectance(0.5f) > 0.5f, "Slab reflectance exceeds single-face (adds second face)");
    }
}
