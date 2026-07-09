#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <vector>
#include <cstdlib>
#include <iostream>

#include "engine/lighting/ShadowMapMath.h"
#include "engine/lighting/LightingProbe.h"
#include "engine/platform/ExceptionMessage.h"

#include "test_expect.h"

#include <stdexcept>
#include <string>

void RunIrradianceShTests(int& failures);
void RunColorSpaceTests(int& failures);
void RunRotationUtilsTests(int& failures);
void RunDxrSettingsTests(int& failures);
void RunDirectionalShadowSettingsTests(int& failures);
void RunDxrAccelerationStructureTests(int& failures);
void RunDxrShaderInfrastructureTests(int& failures);

namespace engine_tests_internal
{
    void ExpectTrue(const bool condition, const char* message)
    {
        test::ExpectTrue(condition, message);
    }

    void ExpectNear(const float actual, const float expected, const float tolerance, const char* message)
    {
        test::ExpectNear(actual, expected, tolerance, message);
    }

    void ExpectContains(const std::string& haystack, const std::string& needle, const char* message)
    {
        test::ExpectContains(haystack, needle, message);
    }

    void RunLightingProbeTests()
    {
        const glm::vec3 lightDirection = glm::normalize(glm::vec3(-0.3f, -1.0f, -0.2f));
        const glm::vec3 upNormal(0.0f, 1.0f, 0.0f);
        const glm::vec3 cubeTop(0.0f, 2.0f, 0.0f);
        const std::vector<float> cascadeSplits = ComputeCascadeSplitDistances(4, 0.1f, 100.0f);
        const float nearPlane = 0.1f;
        const float cascadeBlendRatio = 0.08f;
        const int cascadeCount = 4;

        const float expectedSunDot = glm::dot(upNormal, lightDirection);
        float firstSunDot = 0.0f;
        bool hasFirstSunDot = false;

        for (int step = 0; step < 8; ++step)
        {
            const float angle = glm::two_pi<float>() * (static_cast<float>(step) / 8.0f);
            const glm::vec3 eye(std::cos(angle) * 6.0f, 3.5f, std::sin(angle) * 6.0f);
            const glm::mat4 viewMatrix = glm::lookAtLH(eye, cubeTop, glm::vec3(0.0f, 1.0f, 0.0f));

            const LightingProbeResult probe = EvaluateLightingProbe(
                viewMatrix,
                cubeTop,
                upNormal,
                lightDirection,
                cascadeSplits,
                nearPlane,
                cascadeBlendRatio,
                cascadeCount);

            ExpectNear(probe.sunDotGeomNormal, expectedSunDot, 1e-5f, "Sun dot on cube top should not depend on camera");
            if (!hasFirstSunDot)
            {
                firstSunDot = probe.sunDotGeomNormal;
                hasFirstSunDot = true;
            }
            else
            {
                ExpectNear(
                    probe.sunDotGeomNormal,
                    firstSunDot,
                    1e-5f,
                    "Sun dot on cube top should stay constant across orbit cameras");
            }

            ExpectTrue(probe.viewDepth > nearPlane, "View depth should be beyond the near plane");
        }

        const glm::mat4 nearFloorView = glm::lookAtLH(
            glm::vec3(0.0f, 0.35f, 2.5f),
            glm::vec3(0.0f, 0.0f, 0.0f),
            glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat4 farFloorView = glm::lookAtLH(
            glm::vec3(0.0f, 8.0f, 2.5f),
            glm::vec3(0.0f, 0.0f, 0.0f),
            glm::vec3(0.0f, 1.0f, 0.0f));

        const LightingProbeResult nearFloorProbe = EvaluateLightingProbe(
            nearFloorView,
            glm::vec3(0.0f, 0.0f, 0.0f),
            upNormal,
            lightDirection,
            cascadeSplits,
            nearPlane,
            cascadeBlendRatio,
            cascadeCount);
        const LightingProbeResult farFloorProbe = EvaluateLightingProbe(
            farFloorView,
            glm::vec3(0.0f, 0.0f, 0.0f),
            upNormal,
            lightDirection,
            cascadeSplits,
            nearPlane,
            cascadeBlendRatio,
            cascadeCount);

        ExpectTrue(
            nearFloorProbe.viewDepth < farFloorProbe.viewDepth,
            "Floor probe view depth should decrease as the camera moves closer to the floor");

        ExpectTrue(
            nearFloorProbe.cascadeIndex <= farFloorProbe.cascadeIndex,
            "Closer floor camera should not select a farther cascade than a higher camera");
    }

    void RunExceptionMessageTests()
    {
        ExpectTrue(
            SanitizeLogText("Failed to open project file for reading.") ==
                "Failed to open project file for reading.",
            "ASCII status text should pass through unchanged");

        const std::string binaryPayload(128, static_cast<char>(0xE2));
        const std::string sanitizedBinary = SanitizeLogText(binaryPayload, "Failed to load project.");
        ExpectContains(sanitizedBinary, "non-text payload", "Binary payloads should describe byte content");
        ExpectContains(sanitizedBinary, "128 bytes", "Binary payloads should include byte count");

        try
        {
            throw std::runtime_error("Shader compile failed: assets/shaders/pbr.ps.hlsl");
        }
        catch (const std::exception& exception)
        {
            const std::string message = SafeExceptionMessage(exception);
            ExpectContains(message, "Shader compile failed", "Readable exception messages should survive");
        }

        try
        {
            const std::string message = "Material shader/GPU setup failed: nested detail";
            throw std::runtime_error(message);
        }
        catch (const std::exception& exception)
        {
            const std::string formatted = SafeExceptionMessage(exception);
            ExpectContains(
                formatted,
                "Material shader/GPU setup failed",
                "Owned-string runtime_error messages should survive throw/catch");
        }

        try
        {
            throw std::runtime_error("");
        }
        catch (const std::exception& exception)
        {
            const std::string message = SafeExceptionMessage(exception);
            ExpectContains(message, "runtime_error", "Empty what() should include exception type");
            ExpectContains(message, "empty or unusable", "Empty what() should explain missing message");
        }

        const std::string formatted = FormatExceptionContext("Load", std::runtime_error("bad json"));
        ExpectContains(formatted, "Load:", "Formatted context should include phase prefix");
        ExpectContains(formatted, "bad json", "Formatted context should include exception text");
    }

    void RunShadowMapMathTests()
    {
        const glm::vec3 boundsMin(-6.0f, -0.01f, -6.0f);
        const glm::vec3 boundsMax(6.0f, 3.0f, 6.0f);
        const glm::vec3 lightDirection(0.3f, -0.8f, 0.2f);

        const ShadowLightSpaceSetup setup = BuildShadowLightSpace(
            lightDirection,
            boundsMin,
            boundsMax,
            4096);

        ExpectTrue(setup.orthoWidth > 0.0f, "Ortho width should be positive");
        ExpectTrue(setup.orthoHeight > 0.0f, "Ortho height should be positive");
        ExpectTrue(setup.texelWorldSizeX > 0.0f, "Texel width should be positive");
        ExpectTrue(setup.texelWorldSizeY > 0.0f, "Texel height should be positive");

        const float texelSpan = std::max(setup.texelWorldSizeX, setup.texelWorldSizeY);
        const float facingLightBias = ComputeShadowBias(1.0f, texelSpan);
        const float grazingLightBias = ComputeShadowBias(0.0f, texelSpan);
        ExpectTrue(
            grazingLightBias > facingLightBias,
            "Shadow bias should increase as the surface turns away from the light");

        const float depthSpan = setup.stableOrthoFar - setup.stableOrthoNear;
        const float casterBias =
            ComputeCasterDepthBiasNormalized(texelSpan, setup.stableOrthoNear, setup.stableOrthoFar, 1.0f);
        ExpectTrue(casterBias > 0.0f, "Caster depth bias should be positive");
        ExpectTrue(
            casterBias < texelSpan / std::max(depthSpan, 1e-3f) * 1.01f,
            "Caster depth bias at scale 1 should be about one texel in normalized depth");

        const glm::vec3 cubeCenter(0.0f, 1.5f, 0.0f);
        const glm::vec3 cubeShadowNdc = WorldToShadowNdc(setup.lightSpaceMatrix, cubeCenter);
        ExpectTrue(cubeShadowNdc.x >= 0.0f && cubeShadowNdc.x <= 1.0f, "Cube center UV.x should be inside shadow map");
        ExpectTrue(cubeShadowNdc.y >= 0.0f && cubeShadowNdc.y <= 1.0f, "Cube center UV.y should be inside shadow map");
        ExpectTrue(cubeShadowNdc.z >= 0.0f && cubeShadowNdc.z <= 1.0f, "Cube center depth should be inside shadow map");
        ExpectTrue(
            setup.clipDepthContentMin < setup.clipDepthContentMax,
            "Cascade clip depth content bounds should span a positive range");
        const float contentSpan = setup.clipDepthContentMax - setup.clipDepthContentMin;
        ExpectTrue(contentSpan > 0.5f, "Content depth bounds should span most of clip space for scene bounds");

        const glm::vec3 floorPoint(0.0f, 0.0f, 0.0f);
        const ShadowReceiverProbeResult floorProbe = EvaluateShadowReceiverProbe(
            floorPoint,
            5.0f,
            &setup.lightSpaceMatrix,
            &setup,
            nullptr,
            1);
        ExpectTrue(
            floorProbe.normalizedClipZ >= 0.0f && floorProbe.normalizedClipZ <= 1.0f,
            "Floor probe normalized clip depth should stay in [0, 1]");

        const float snapMagnitude = glm::length(setup.snapOffsetNdc);
        ExpectTrue(
            snapMagnitude < (2.0f / 4096.0f),
            "Texel snap offset should be smaller than one shadow texel in NDC");

        const std::vector<float> cascadeSplits = ComputeCascadeSplitDistances(4, 0.1f, 100.0f);
        ExpectTrue(cascadeSplits.size() == 5U, "Four cascades should produce five split boundaries");
        ExpectNear(cascadeSplits.front(), 0.1f, 1e-4f, "First cascade split should start at the near plane");
        ExpectNear(cascadeSplits.back(), 100.0f, 1e-4f, "Last cascade split should end at the far plane");
        ExpectTrue(
            cascadeSplits[1] > cascadeSplits[0] && cascadeSplits[2] > cascadeSplits[1] &&
                cascadeSplits[3] > cascadeSplits[2],
            "Cascade split distances should be monotonically increasing");

        const glm::mat4 inverseView = glm::inverse(glm::lookAt(
            glm::vec3(0.0f, 2.0f, 6.0f),
            glm::vec3(0.0f, 1.0f, 0.0f),
            glm::vec3(0.0f, 1.0f, 0.0f)));
        const std::array<glm::vec3, 8> nearFrustumCorners = ComputeCascadeFrustumCorners(
            inverseView,
            16.0f / 9.0f,
            45.0f,
            cascadeSplits[0],
            cascadeSplits[1]);
        const std::array<glm::vec3, 8> farFrustumCorners = ComputeCascadeFrustumCorners(
            inverseView,
            16.0f / 9.0f,
            45.0f,
            cascadeSplits[2],
            cascadeSplits[3]);

        const ShadowLightSpaceSetup nearCascadeSetup = BuildShadowLightSpace(
            lightDirection,
            ComputeBoundsMin(nearFrustumCorners),
            ComputeBoundsMax(nearFrustumCorners),
            4096);
        const ShadowLightSpaceSetup farCascadeSetup = BuildShadowLightSpace(
            lightDirection,
            ComputeBoundsMin(farFrustumCorners),
            ComputeBoundsMax(farFrustumCorners),
            4096);

        ExpectTrue(
            nearCascadeSetup.texelWorldSizeX < farCascadeSetup.texelWorldSizeX,
            "Near cascade texels should be smaller than the farthest cascade");

        const glm::vec3 casterBoundsMin(-20.0f, 0.0f, -20.0f);
        const glm::vec3 casterBoundsMax(20.0f, 10.0f, 20.0f);
        const ShadowLightSpaceSetup nearFrustumCascadeSetup = BuildShadowLightSpaceForFrustumCorners(
            lightDirection,
            nearFrustumCorners,
            4096,
            0.03f,
            0.12f,
            &casterBoundsMin,
            &casterBoundsMax);
        const ShadowLightSpaceSetup farFrustumCascadeSetup = BuildShadowLightSpaceForFrustumCorners(
            lightDirection,
            farFrustumCorners,
            4096,
            0.03f,
            0.12f,
            &casterBoundsMin,
            &casterBoundsMax);
        ExpectTrue(
            nearFrustumCascadeSetup.clipDepthContentMin != farFrustumCascadeSetup.clipDepthContentMin ||
                nearFrustumCascadeSetup.clipDepthContentMax != farFrustumCascadeSetup.clipDepthContentMax,
            "Near and far cascade frustum debug depth ranges should differ");
        ExpectTrue(
            nearFrustumCascadeSetup.orthoWidth < farFrustumCascadeSetup.orthoWidth,
            "Near cascade ortho should be tighter than far cascade with frustum-only XY fit");

        const ShadowLightSpaceSetup frustumOnlySetup = BuildShadowLightSpaceForFrustumCorners(
            lightDirection,
            nearFrustumCorners,
            4096,
            0.03f,
            0.12f,
            nullptr,
            nullptr);
        const ShadowLightSpaceSetup frustumWithCasterSetup = nearFrustumCascadeSetup;

        ExpectNear(
            frustumOnlySetup.orthoWidth,
            frustumWithCasterSetup.orthoWidth,
            1e-3f,
            "Caster bounds should not expand ortho XY (frustum-only fit is always on)");
        ExpectNear(
            frustumOnlySetup.texelWorldSizeX,
            frustumWithCasterSetup.texelWorldSizeX,
            1e-6f,
            "Caster bounds should not change cascade texel size in XY");
        ExpectTrue(
            frustumWithCasterSetup.stableOrthoNear != frustumOnlySetup.stableOrthoNear ||
                frustumWithCasterSetup.stableOrthoFar != frustumOnlySetup.stableOrthoFar,
            "Intersecting caster bounds that extend frustum depth should affect ortho Z");

        const glm::vec3 distantCasterMin(-20.0f, -100.0f, -20.0f);
        const glm::vec3 distantCasterMax(20.0f, -90.0f, 20.0f);
        const ShadowLightSpaceSetup distantCasterSetup = BuildShadowLightSpaceForFrustumCorners(
            lightDirection,
            nearFrustumCorners,
            4096,
            0.03f,
            0.12f,
            &distantCasterMin,
            &distantCasterMax);
        ExpectTrue(
            distantCasterSetup.stableOrthoNear != frustumOnlySetup.stableOrthoNear ||
                distantCasterSetup.stableOrthoFar != frustumOnlySetup.stableOrthoFar,
            "Non-intersecting caster bounds should expand ortho Z range");
        ExpectTrue(
            (distantCasterSetup.stableOrthoFar - distantCasterSetup.stableOrthoNear) >
                (frustumOnlySetup.stableOrthoFar - frustumOnlySetup.stableOrthoNear),
            "Non-intersecting caster bounds should widen ortho depth span");

        const glm::mat4 nearCameraView = glm::lookAt(
            glm::vec3(0.0f, 2.0f, 6.0f),
            glm::vec3(0.0f, 1.0f, 0.0f),
            glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat4 dollyCameraView = glm::lookAt(
            glm::vec3(0.0f, 2.0f, 4.5f),
            glm::vec3(0.0f, 1.0f, 0.0f),
            glm::vec3(0.0f, 1.0f, 0.0f));

        const std::array<glm::vec3, 8> dollyFrustumCorners = ComputeCascadeFrustumCorners(
            glm::inverse(dollyCameraView),
            16.0f / 9.0f,
            45.0f,
            cascadeSplits[0],
            cascadeSplits[1]);

        const ShadowLightSpaceSetup stablePassSetup = BuildShadowLightSpaceForFrustumCorners(
            lightDirection,
            nearFrustumCorners,
            4096,
            0.03f,
            0.12f,
            &casterBoundsMin,
            &casterBoundsMax);
        const ShadowLightSpaceSetup stableRetainedSetup = BuildShadowLightSpaceForFrustumCorners(
            lightDirection,
            nearFrustumCorners,
            4096,
            0.03f,
            0.12f,
            &casterBoundsMin,
            &casterBoundsMax);

        const glm::vec3 floorShadowNdcStart = WorldToShadowNdc(stablePassSetup.lightSpaceMatrix, floorPoint);
        const glm::vec3 floorShadowNdcEnd = WorldToShadowNdc(stableRetainedSetup.lightSpaceMatrix, floorPoint);
        ExpectNear(
            floorShadowNdcStart.x,
            floorShadowNdcEnd.x,
            1e-5f,
            "Floor shadow UV.x should stay stable when the frustum is unchanged");
        ExpectNear(
            floorShadowNdcStart.y,
            floorShadowNdcEnd.y,
            1e-5f,
            "Floor shadow UV.y should stay stable when the frustum is unchanged");

        const float contentClipSpan =
            stablePassSetup.clipDepthContentMax - stablePassSetup.clipDepthContentMin;
        ExpectTrue(
            contentClipSpan > 0.05f,
            "Cascade content clip-Z span should use a meaningful slice of [0, 1]");

        const glm::vec3 cubeTopPoint(0.0f, 2.0f, 0.0f);
        const float floorClipZ =
            WorldToShadowSampleCoords(stablePassSetup.lightSpaceMatrix, floorPoint).z;
        const float cubeTopClipZ =
            WorldToShadowSampleCoords(stablePassSetup.lightSpaceMatrix, cubeTopPoint).z;
        ExpectTrue(
            std::abs(floorClipZ - cubeTopClipZ) > 0.02f,
            "Separated world points should map to distinct light-space clip Z values");

        const ShadowReceiverProbeResult cascadeFloorProbe = EvaluateShadowReceiverProbe(
            floorPoint,
            5.0f,
            &stablePassSetup.lightSpaceMatrix,
            &stablePassSetup,
            nullptr,
            1);
        ExpectTrue(
            cascadeFloorProbe.normalizedClipZ >= 0.0f && cascadeFloorProbe.normalizedClipZ <= 1.0f,
            "Floor normalized clip depth should stay in [0, 1]");
        ExpectTrue(
            cascadeFloorProbe.normalizedClipZ > 0.05f && cascadeFloorProbe.normalizedClipZ < 0.95f,
            "Floor normalized clip depth should land in the mid-range of the content band");

        const ShadowLightSpaceSetup dollyFrustumSetup = BuildShadowLightSpaceForFrustumCorners(
            lightDirection,
            dollyFrustumCorners,
            4096,
            0.03f,
            0.12f,
            &casterBoundsMin,
            &casterBoundsMax);
        ExpectNear(
            stablePassSetup.lightView[0][0],
            dollyFrustumSetup.lightView[0][0],
            1e-5f,
            "Light view should not change when the camera dollies");
        ExpectNear(
            stablePassSetup.lightView[3][0],
            dollyFrustumSetup.lightView[3][0],
            1e-5f,
            "Light view translation should stay anchored to world origin");

        const glm::mat4 rotatedCameraView = glm::lookAt(
            glm::vec3(6.0f, 2.0f, 0.0f),
            glm::vec3(0.0f, 1.0f, 0.0f),
            glm::vec3(0.0f, 1.0f, 0.0f));
        const std::array<glm::vec3, 8> rotatedFrustumCorners = ComputeCascadeFrustumCorners(
            glm::inverse(rotatedCameraView),
            16.0f / 9.0f,
            45.0f,
            cascadeSplits[0],
            cascadeSplits[1]);

        const ShadowLightSpaceSetup rotatedFrustumSetup = BuildShadowLightSpaceForFrustumCorners(
            lightDirection,
            rotatedFrustumCorners,
            4096,
            0.03f,
            0.12f,
            &casterBoundsMin,
            &casterBoundsMax);

        const ShadowLightSpaceSetup globalCasterSetup =
            BuildShadowLightSpace(lightDirection, casterBoundsMin, casterBoundsMax, 4096);
        ExpectTrue(rotatedFrustumSetup.orthoWidth > 0.0f, "Rotated cascade ortho should be valid");
        ExpectTrue(
            rotatedFrustumSetup.orthoWidth < globalCasterSetup.orthoWidth,
            "Rotated cascade ortho should stay tighter than global caster bounds");
        const float rotatedContentClipSpan =
            rotatedFrustumSetup.clipDepthContentMax - rotatedFrustumSetup.clipDepthContentMin;
        ExpectTrue(
            rotatedContentClipSpan > 0.05f,
            "Rotated cascade content clip-Z span should use a meaningful slice of [0, 1]");

        glm::vec3 intersectionMin;
        glm::vec3 intersectionMax;
        ExpectTrue(
            ComputeBoundsIntersection(
                glm::vec3(-2.0f, 0.0f, -2.0f),
                glm::vec3(2.0f, 4.0f, 2.0f),
                glm::vec3(0.0f, 0.0f, 0.0f),
                glm::vec3(6.0f, 2.0f, 6.0f),
                intersectionMin,
                intersectionMax),
            "Overlapping bounds should intersect");
        ExpectNear(intersectionMin.x, 0.0f, 1e-4f, "Intersection min X");
        ExpectNear(intersectionMax.y, 2.0f, 1e-4f, "Intersection max Y");
    }

    void RunAllEngineTests()
    {
        test::RunTest("shadow_map_math", RunShadowMapMathTests);
        test::RunTest("lighting_probe", RunLightingProbeTests);
        test::RunTest("exception_message", RunExceptionMessageTests);
        test::RunTest("irradiance_sh", [] { RunIrradianceShTests(test::FailureCount()); });
        test::RunTest("color_space", [] { RunColorSpaceTests(test::FailureCount()); });
        test::RunTest("rotation_utils", [] { RunRotationUtilsTests(test::FailureCount()); });
        test::RunTest("dxr_settings", [] { RunDxrSettingsTests(test::FailureCount()); });
        test::RunTest(
            "directional_shadow_settings",
            [] { RunDirectionalShadowSettingsTests(test::FailureCount()); });
        test::RunTest(
            "dxr_acceleration_structure",
            [] { RunDxrAccelerationStructureTests(test::FailureCount()); });
        test::RunTest(
            "dxr_shader_infrastructure",
            [] { RunDxrShaderInfrastructureTests(test::FailureCount()); });
    }
}

int main()
{
    test::ResetFailures();
    test::ResetTestRun();
    engine_tests_internal::RunAllEngineTests();
    test::PrintSummary();
    return test::ExitCode();
}
