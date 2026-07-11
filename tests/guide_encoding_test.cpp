// CPU mirrors of guide / motion math from assets/shaders/dxr/path_tracer.hlsl and
// assets/shaders/dxr/pt_dielectric.hlsli. Keep in sync when shader formulas change.

#include "test_expect.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace
{
    float Saturate(float value)
    {
        return std::clamp(value, 0.0f, 1.0f);
    }

    glm::vec3 Saturate(const glm::vec3& value)
    {
        return glm::clamp(value, 0.0f, 1.0f);
    }

    float DielectricWeight(float transmission, float metallic)
    {
        return Saturate(transmission) * (1.0f - Saturate(metallic));
    }

    glm::vec2 PixelToClipXY(const glm::vec2& texCoord)
    {
        return glm::vec2(texCoord.x * 2.0f - 1.0f, 1.0f - texCoord.y * 2.0f);
    }

    glm::vec2 ComputeMotionNdc(const glm::vec4& currClip, const glm::vec4& prevClip)
    {
        const glm::vec2 currNdc(currClip.x / currClip.w, currClip.y / currClip.w);
        const glm::vec2 prevNdc(prevClip.x / prevClip.w, prevClip.y / prevClip.w);
        return currNdc - prevNdc;
    }

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
        return 0.5f * (rPar * rPar + rPerp * rPerp);
    }

    glm::vec3 EnvBRDFApprox2(const glm::vec3& specularColor, float alpha, float nDotV)
    {
        const float noV = std::abs(nDotV);
        const glm::vec4 x(1.0f, noV, noV * noV, noV * noV * noV);
        const glm::vec4 y(1.0f, alpha, alpha * alpha, alpha * alpha * alpha);

        const glm::mat2 m1(0.99044f, -1.28514f, 1.29678f, -0.755907f);
        const glm::mat3 m2(
            1.0f,
            2.92338f,
            59.4188f,
            20.3225f,
            -27.0302f,
            222.592f,
            121.563f,
            626.13f,
            316.627f);
        const glm::mat2 m3(0.0365463f, 3.32707f, 9.0632f, -9.04756f);
        const glm::mat3 m4(
            1.0f,
            3.59685f,
            -1.36772f,
            9.04401f,
            -16.3174f,
            9.22949f,
            5.56589f,
            19.7886f,
            -20.2123f);

        const float biasNumerator = glm::dot(m1 * glm::vec2(x.x, x.y), glm::vec2(y.x, y.y));
        const float biasDenominator = glm::dot(m2 * glm::vec3(x.x, x.y, x.w), glm::vec3(y.x, y.y, y.w));
        const float scaleNumerator = glm::dot(m3 * glm::vec2(x.x, x.y), glm::vec2(y.x, y.y));
        const float scaleDenominator =
            glm::dot(m4 * glm::vec3(x.x, x.z, x.w), glm::vec3(y.x, y.y, y.w));

        float bias = biasNumerator / biasDenominator;
        float scale = scaleNumerator / scaleDenominator;
        bias *= Saturate(specularColor.g * 50.0f);
        return specularColor * std::max(0.0f, scale) + glm::vec3(std::max(0.0f, bias));
    }

    struct GuideMaterial
    {
        glm::vec3 albedo{0.8f, 0.8f, 0.8f};
        float roughness = 0.2f;
        float metallic = 0.0f;
        float transmission = 0.0f;
        float indexOfRefraction = 1.5f;
    };

    // Mirror of ComputePtPrimaryRrMaterialGuides in path_tracer.hlsl — NVIDIA-canonical semantics
    // (ProgrammingGuideDLSS_RR.md §4.2.1): diffuse = albedo·(1−metallic) (black on metals), spec =
    // EnvBRDFApprox2(F0, roughness², NoV). Settled by the 2026-07-11 guide 2x2 in
    // rr-gi-diagnosis.md §E3 (metal diffuse lean -> boil; raw-F0 spec -> magenta fringe).
    void ComputePtPrimaryRrMaterialGuides(
        const GuideMaterial& material,
        const glm::vec3& hitNormal,
        const glm::vec3& viewDir,
        glm::vec3& diffuseGuide,
        glm::vec3& specGuide,
        glm::vec3& guideNormal,
        float& guideRoughness)
    {
        const glm::vec3 albedo = material.albedo;
        const glm::vec3 f0 = glm::mix(glm::vec3(0.04f), albedo, material.metallic);
        const float dielectricWeight = DielectricWeight(material.transmission, material.metallic);
        const float nDotV = Saturate(glm::dot(hitNormal, viewDir));

        diffuseGuide = albedo * (1.0f - material.metallic) * (1.0f - dielectricWeight);

        const float dielectricSpec =
            FresnelDielectric(nDotV, 1.0f / std::max(material.indexOfRefraction, 1.0f));
        const glm::vec3 opaqueSpecGuide = glm::max(
            EnvBRDFApprox2(f0, material.roughness * material.roughness, nDotV),
            glm::vec3(0.04f));
        specGuide = glm::mix(
            opaqueSpecGuide,
            glm::vec3(dielectricSpec),
            dielectricWeight);
        guideNormal = glm::normalize(hitNormal);
        guideRoughness = material.roughness;
    }

    glm::vec2 ComputeSkyAnchorMotion(
        const glm::vec3& cameraPos,
        const glm::vec3& anchorDirection,
        float maxTraceDistance,
        const glm::mat4& currViewProj,
        const glm::mat4& prevViewProj,
        bool historyValid)
    {
        const glm::vec3 skyAnchor = cameraPos + anchorDirection * (maxTraceDistance * 0.5f);
        glm::vec4 currClipUnj = currViewProj * glm::vec4(skyAnchor, 1.0f);
        glm::vec4 prevClipUnj = prevViewProj * glm::vec4(skyAnchor, 1.0f);
        if (!historyValid)
        {
            prevClipUnj = currClipUnj;
        }

        return ComputeMotionNdc(currClipUnj, prevClipUnj);
    }

    // CPU mirror of bounce-0 transmission guide material selection in path_tracer.hlsl.
    struct TransmissionGuideHitCpu
    {
        bool valid = false;
        glm::vec3 backgroundNormal{0.0f, 0.0f, 1.0f};
        glm::vec2 backgroundMotion{0.0f};
        glm::vec2 virtualMotion{0.0f};
    };

    void ApplyTransmissionMaterialGuideSelection(
        const GuideMaterial& surfaceMaterial,
        const GuideMaterial& backgroundMaterial,
        const glm::vec3& surfaceNormal,
        const glm::vec3& backgroundNormal,
        const glm::vec3& viewDir,
        const glm::vec2& surfaceMotion,
        const TransmissionGuideHitCpu& txGuide,
        glm::vec3& diffuseGuide,
        glm::vec3& specGuide,
        glm::vec3& guideNormal,
        float& guideRoughness,
        glm::vec2& motionGuide)
    {
        ComputePtPrimaryRrMaterialGuides(
            surfaceMaterial,
            surfaceNormal,
            viewDir,
            diffuseGuide,
            specGuide,
            guideNormal,
            guideRoughness);
        motionGuide = surfaceMotion;

        const float dielectricWeight =
            DielectricWeight(surfaceMaterial.transmission, surfaceMaterial.metallic);
        if (dielectricWeight <= 0.01f)
        {
            return;
        }

        const float nDotV = Saturate(glm::dot(surfaceNormal, viewDir));
        const float transmitFresnel = FresnelDielectric(
            nDotV,
            1.0f / std::max(surfaceMaterial.indexOfRefraction, 1.0f));
        const float transmitWeight = Saturate(1.0f - transmitFresnel);

        if (txGuide.valid)
        {
            glm::vec3 bgDiffuse{};
            glm::vec3 bgSpec{};
            glm::vec3 bgNormal{};
            float bgRoughness = 0.0f;
            const glm::vec3 bgViewDir = glm::normalize(-viewDir);
            ComputePtPrimaryRrMaterialGuides(
                backgroundMaterial,
                backgroundNormal,
                bgViewDir,
                bgDiffuse,
                bgSpec,
                bgNormal,
                bgRoughness);

            diffuseGuide = glm::mix(diffuseGuide, bgDiffuse, transmitWeight);
            specGuide = glm::mix(specGuide, bgSpec, transmitWeight);
            guideNormal = glm::normalize(glm::mix(guideNormal, bgNormal, transmitWeight));
            guideRoughness = glm::mix(guideRoughness, bgRoughness, transmitWeight);
            motionGuide = glm::mix(surfaceMotion, txGuide.virtualMotion, transmitWeight);
            return;
        }

        const glm::vec3 skyGuide(0.5f);
        diffuseGuide = glm::mix(diffuseGuide, skyGuide, transmitWeight);
        specGuide = glm::mix(specGuide, skyGuide, transmitWeight);
        motionGuide = glm::mix(surfaceMotion, txGuide.virtualMotion, transmitWeight);
    }

    glm::vec2 ComputeVirtualRefractedMotionFromWorldPoints(
        const glm::mat4& currUnjitteredViewProj,
        const glm::mat4& prevViewProj,
        const glm::vec3& worldCurr,
        const glm::vec3& worldPrev)
    {
        const glm::vec4 currClipUnj = currUnjitteredViewProj * glm::vec4(worldCurr, 1.0f);
        const glm::vec4 prevClipUnj = prevViewProj * glm::vec4(worldPrev, 1.0f);
        return ComputeMotionNdc(currClipUnj, prevClipUnj);
    }
}

void RunGuideEncodingTests()
{
    {
        test::ExpectNear(DielectricWeight(1.0f, 0.0f), 1.0f, 1e-6f, "Full transmission dielectric weight");
        test::ExpectNear(DielectricWeight(1.0f, 1.0f), 0.0f, 1e-6f, "Metallic cancels dielectric weight");
        test::ExpectNear(DielectricWeight(-0.5f, 0.0f), 0.0f, 1e-6f, "Negative transmission clamps to 0");
    }

    {
        const glm::vec2 topLeft = PixelToClipXY(glm::vec2(0.0f, 0.0f));
        test::ExpectNear(topLeft.x, -1.0f, 1e-6f, "PixelToClipXY top-left X");
        test::ExpectNear(topLeft.y, 1.0f, 1e-6f, "PixelToClipXY top-left Y");

        const glm::vec2 center = PixelToClipXY(glm::vec2(0.5f, 0.5f));
        test::ExpectNear(center.x, 0.0f, 1e-6f, "PixelToClipXY center X");
        test::ExpectNear(center.y, 0.0f, 1e-6f, "PixelToClipXY center Y");
    }

    {
        const glm::vec4 currClip(1.2f, 0.4f, 0.5f, 2.0f);
        const glm::vec4 prevClip(0.8f, 0.4f, 0.5f, 2.0f);
        const glm::vec2 motion = ComputeMotionNdc(currClip, prevClip);
        test::ExpectNear(motion.x, 0.2f, 1e-6f, "Motion NDC X delta");
        test::ExpectNear(motion.y, 0.0f, 1e-6f, "Motion NDC Y unchanged");
    }

    {
        GuideMaterial opaque{};
        opaque.albedo = glm::vec3(0.6f, 0.1f, 0.2f);
        opaque.roughness = 0.35f;
        opaque.transmission = 0.0f;

        glm::vec3 diffuseGuide{};
        glm::vec3 specGuide{};
        glm::vec3 guideNormal{};
        float guideRoughness = 0.0f;
        const glm::vec3 normal(0.0f, 1.0f, 0.0f);
        const glm::vec3 viewDir = glm::normalize(glm::vec3(0.2f, 0.9f, 0.3f));

        ComputePtPrimaryRrMaterialGuides(
            opaque,
            normal,
            viewDir,
            diffuseGuide,
            specGuide,
            guideNormal,
            guideRoughness);

        test::ExpectNear(diffuseGuide.r, opaque.albedo.r, 1e-4f, "Opaque diffuse guide R");
        test::ExpectTrue(specGuide.r >= 0.04f, "Opaque spec guide floors at 0.04 (EnvBRDFApprox2)");
        test::ExpectNear(guideRoughness, opaque.roughness, 1e-6f, "Guide roughness passthrough");
    }

    {
        // Metal guide semantics guard (rr-gi-diagnosis.md §E3, 2026-07-11 guide 2x2): metals emit
        // BLACK diffuse (a 0.5 lean routed the metal's noisy specular GI into RR's diffuse
        // accumulation -> intense boil on metals) and EnvBRDFApprox2 spec (raw F0 under-attributed
        // rough-metal reflectance -> magenta silhouette fringe). NVIDIA-canonical per
        // ProgrammingGuideDLSS_RR.md §4.2.1.
        GuideMaterial metal{};
        metal.albedo = glm::vec3(0.1f, 0.7f, 0.15f); // green metal (the repro capsule)
        metal.roughness = 0.3f;
        metal.metallic = 1.0f;

        glm::vec3 diffuseGuide{};
        glm::vec3 specGuide{};
        glm::vec3 guideNormal{};
        float guideRoughness = 0.0f;
        const glm::vec3 normal(0.0f, 1.0f, 0.0f);
        const glm::vec3 viewDir = glm::normalize(glm::vec3(0.1f, 1.0f, 0.2f));

        ComputePtPrimaryRrMaterialGuides(
            metal,
            normal,
            viewDir,
            diffuseGuide,
            specGuide,
            guideNormal,
            guideRoughness);

        test::ExpectNear(diffuseGuide.r, 0.0f, 1e-4f, "Metal diffuse guide is black R");
        test::ExpectNear(diffuseGuide.g, 0.0f, 1e-4f, "Metal diffuse guide is black G");
        // EnvBRDFApprox2 rescales F0 (scale may be < 1 at some roughness/NoV; bias adds back) —
        // assert the guide stays colored like the metal and above the 0.04 floor, not a strict lift.
        test::ExpectTrue(specGuide.g >= 0.04f, "Metal spec guide respects the 0.04 floor");
        test::ExpectTrue(
            specGuide.g > specGuide.r && specGuide.g > specGuide.b,
            "Green metal spec guide stays green-dominant (carries the metal color)");
        test::ExpectTrue(specGuide.g <= 1.0f, "Metal spec guide stays plausible (<= 1)");
    }

    {
        GuideMaterial glass{};
        glass.albedo = glm::vec3(1.0f);
        glass.transmission = 1.0f;
        glass.indexOfRefraction = 1.5f;

        glm::vec3 diffuseGuide{};
        glm::vec3 specGuide{};
        glm::vec3 guideNormal{};
        float guideRoughness = 0.0f;
        const glm::vec3 normal(0.0f, 1.0f, 0.0f);
        const glm::vec3 viewDir = glm::normalize(glm::vec3(0.0f, 1.0f, 0.0f));

        ComputePtPrimaryRrMaterialGuides(
            glass,
            normal,
            viewDir,
            diffuseGuide,
            specGuide,
            guideNormal,
            guideRoughness);

        test::ExpectNear(diffuseGuide.r, 0.0f, 1e-4f, "Glass diffuse guide suppressed by dielectric weight");
        test::ExpectTrue(specGuide.r < 0.1f, "Glass spec guide should use Fresnel dielectric at normal incidence");
        test::ExpectTrue(specGuide.r > 0.0f, "Glass spec guide should remain non-zero");
    }

    {
        const glm::vec3 cameraPos(0.0f, 0.0f, 5.0f);
        const glm::vec3 anchorDirection = glm::normalize(glm::vec3(0.0f, 0.2f, 1.0f));
        const float maxTraceDistance = 100.0f;

        const glm::mat4 currView = glm::lookAt(cameraPos, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat4 currProj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
        const glm::mat4 currViewProj = currProj * currView;

        const glm::vec3 prevCameraPos(0.5f, 0.0f, 5.0f);
        const glm::mat4 prevView = glm::lookAt(prevCameraPos, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat4 prevViewProj = currProj * prevView;

        const glm::vec2 motion = ComputeSkyAnchorMotion(
            cameraPos,
            anchorDirection,
            maxTraceDistance,
            currViewProj,
            prevViewProj,
            true);

        test::ExpectTrue(std::abs(motion.x) > 1e-4f, "Sky anchor motion X should be non-zero when camera pans");
    }

    {
        GuideMaterial glass{};
        glass.transmission = 1.0f;
        glass.indexOfRefraction = 1.5f;

        GuideMaterial backdrop{};
        backdrop.albedo = glm::vec3(0.85f, 0.12f, 0.08f);
        backdrop.roughness = 0.35f;

        const glm::vec3 glassNormal(0.0f, 0.0f, 1.0f);
        const glm::vec3 backdropNormal(0.0f, 0.0f, 1.0f);
        const glm::vec3 viewDir(0.0f, 0.0f, 1.0f);

        TransmissionGuideHitCpu txGuide{};
        txGuide.valid = true;
        txGuide.backgroundNormal = backdropNormal;
        txGuide.virtualMotion = glm::vec2(0.01f, 0.0f);

        glm::vec3 diffuseGuide{};
        glm::vec3 specGuide{};
        glm::vec3 guideNormal{};
        float guideRoughness = 0.0f;
        glm::vec2 motionGuide{};

        ApplyTransmissionMaterialGuideSelection(
            glass,
            backdrop,
            glassNormal,
            backdropNormal,
            viewDir,
            glm::vec2(0.0f),
            txGuide,
            diffuseGuide,
            specGuide,
            guideNormal,
            guideRoughness,
            motionGuide);

        test::ExpectTrue(diffuseGuide.r > 0.7f, "Transmission guide should pick backdrop diffuse albedo R");
        test::ExpectTrue(diffuseGuide.g < 0.25f, "Transmission guide should pick backdrop diffuse albedo G");
        test::ExpectNear(guideNormal.z, backdropNormal.z, 1e-4f, "Transmission guide normal should lean to backdrop");
        test::ExpectTrue(std::abs(motionGuide.x) > 1e-4f, "Transmission guide motion should use virtual refracted MV");
    }

    {
        GuideMaterial opaque{};
        opaque.albedo = glm::vec3(0.2f, 0.7f, 0.1f);
        opaque.transmission = 0.0f;

        GuideMaterial backdrop{};
        backdrop.albedo = glm::vec3(1.0f, 0.0f, 0.0f);

        const glm::vec3 normal(0.0f, 1.0f, 0.0f);
        const glm::vec3 viewDir = glm::normalize(glm::vec3(0.0f, 1.0f, 0.0f));

        TransmissionGuideHitCpu txGuide{};
        txGuide.valid = true;

        glm::vec3 diffuseGuide{};
        glm::vec3 specGuide{};
        glm::vec3 guideNormal{};
        float guideRoughness = 0.0f;
        glm::vec2 motionGuide{};

        ApplyTransmissionMaterialGuideSelection(
            opaque,
            backdrop,
            normal,
            normal,
            viewDir,
            glm::vec2(0.0f),
            txGuide,
            diffuseGuide,
            specGuide,
            guideNormal,
            guideRoughness,
            motionGuide);

        test::ExpectNear(diffuseGuide.r, opaque.albedo.r, 1e-4f, "Opaque surface ignores transmission guide backdrop");
    }

    {
        GuideMaterial glass{};
        glass.transmission = 1.0f;

        const glm::vec3 glassNormal(0.0f, 0.0f, 1.0f);
        const glm::vec3 viewDir(0.0f, 0.0f, 1.0f);

        TransmissionGuideHitCpu txGuide{};
        txGuide.valid = false;
        txGuide.virtualMotion = glm::vec2(0.02f, 0.0f);

        GuideMaterial unusedBackdrop{};
        glm::vec3 diffuseGuide{};
        glm::vec3 specGuide{};
        glm::vec3 guideNormal{};
        float guideRoughness = 0.0f;
        glm::vec2 motionGuide{};

        ApplyTransmissionMaterialGuideSelection(
            glass,
            unusedBackdrop,
            glassNormal,
            glassNormal,
            viewDir,
            glm::vec2(0.0f),
            txGuide,
            diffuseGuide,
            specGuide,
            guideNormal,
            guideRoughness,
            motionGuide);

        test::ExpectNear(diffuseGuide.r, 0.5f, 0.05f, "Guide miss should fall back to neutral sky albedo");
        test::ExpectTrue(std::abs(motionGuide.x) > 1e-4f, "Guide miss should still use virtual motion");
    }

    {
        const glm::mat4 currView = glm::lookAt(glm::vec3(0.0f, 0.0f, 4.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat4 currProj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
        const glm::mat4 currUnjViewProj = currProj * currView;

        const glm::mat4 prevView =
            glm::lookAt(glm::vec3(0.0f, 0.0f, 4.0f), glm::vec3(0.5f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat4 prevViewProj = currProj * prevView;

        const glm::vec3 worldCurr(0.0f, 0.0f, -1.0f);
        const glm::vec3 worldPrev(0.15f, 0.0f, -1.0f);

        const glm::vec2 motion = ComputeVirtualRefractedMotionFromWorldPoints(
            currUnjViewProj,
            prevViewProj,
            worldCurr,
            worldPrev);

        test::ExpectTrue(glm::length(motion) > 0.002f, "Virtual refracted MV should be non-zero under camera orbit");
    }
}
