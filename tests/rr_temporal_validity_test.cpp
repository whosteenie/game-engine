#include "engine/rendering/post/RrTemporalValidity.h"
#include "engine/rhi/DlssContext.h"
#include "test_expect.h"

#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>

void RunRrTemporalValidityTests(int& failures)
{
    (void)failures;
    using namespace RrTemporalValidity;
    const auto expect = [&](const bool value, const char* message)
    {
        test::ExpectTrue(value, message);
    };

    // Triangulation never participates in the key: one instance remains one stable owner.
    const std::uint32_t planarTriangleA = SurfaceOwner(17u);
    const std::uint32_t planarTriangleB = SurfaceOwner(17u);
    expect(planarTriangleA == planarTriangleB, "surface owner must be triangulation-stable");
    expect(planarTriangleA != SurfaceOwner(18u), "different instances need different owners");

    const std::uint32_t chain01 = Mix(Mix(MirrorDomain, 1u), 2u);
    const std::uint32_t chain10 = Mix(Mix(MirrorDomain, 2u), 1u);
    expect(chain01 != chain10, "mirror-chain owner must preserve path order");
    expect(SkyDomain != planarTriangleA, "sky owns an explicit non-surface domain");

    const Uv historyUv = Reproject({0.5f, 0.5f}, {0.2f, -0.4f});
    expect(std::abs(historyUv.x - 0.4f) < 1.0e-6f
            && std::abs(historyUv.y - 0.3f) < 1.0e-6f,
        "CPU reprojection must convert current-minus-previous NDC into previous texture UV");
    const Uv jitteredHistoryUv = Reproject(
        {0.5f, 0.5f},
        {0.0f, 0.0f},
        {-0.5f, 0.5f},
        {0.001f, -0.002f},
        {-0.003f, 0.004f});
    expect(std::abs(jitteredHistoryUv.x - 0.498f) < 1.0e-6f
            && std::abs(jitteredHistoryUv.y - 0.497f) < 1.0e-6f,
        "static reprojection must move between current and previous jittered pixel lattices");
    expect(Classify(false, historyUv, 1u, 1u, 0.9f, 0.9f, 1.0f)
            == InvalidHistoryOrUv,
        "invalid history rejects completely");
    expect((Classify(true, historyUv, 1u, 2u, 0.9f, 0.9f, 1.0f)
            & OwnerMismatch) != 0u,
        "owner changes reject history");
    expect((Classify(true, historyUv, 1u, 1u, 0.9f, 0.95f, 1.0f)
            & DepthMismatch) != 0u,
        "relative depth discontinuities reject history");
    expect((Classify(true, historyUv, 1u, 1u, 0.9f, 0.9f, 0.5f)
            & NormalMismatch) != 0u,
        "incompatible normals reject history");

    // Current device depth is not the expected previous-frame depth when the camera changes.
    // clipToPrevClip must transform the current homogeneous point into the history depth domain.
    const glm::mat4 currentViewProjection(1.0f);
    const glm::mat4 previousViewProjection = glm::translate(
        glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 0.2f));
    const glm::vec4 worldPoint(0.1f, -0.2f, 0.4f, 1.0f);
    const glm::vec4 currentClip = currentViewProjection * worldPoint;
    const glm::mat4 clipToPrevious =
        previousViewProjection * glm::inverse(currentViewProjection);
    const glm::vec4 expectedPreviousClip = clipToPrevious * currentClip;
    const glm::vec4 directPreviousClip = previousViewProjection * worldPoint;
    const float expectedPreviousDepth = expectedPreviousClip.z / expectedPreviousClip.w;
    const float directPreviousDepth = directPreviousClip.z / directPreviousClip.w;
    expect(std::abs(expectedPreviousDepth - directPreviousDepth) < 1.0e-6f,
        "clip-to-previous transform must reproduce the history depth domain");
    expect(DeviceDepthResidual(expectedPreviousDepth, directPreviousDepth) < 1.0e-6f
            && DeviceDepthResidual(currentClip.z / currentClip.w, directPreviousDepth) > 0.02f,
        "depth validity must compare expected previous depth, not current view depth");

    DlssFrameInputs primary{};
    primary.useRayReconstruction = true;
    primary.viewportId = 7u;
    primary.specularHitDistance = reinterpret_cast<void*>(1);
    primary.responsivityMask = reinterpret_cast<void*>(2);
    const DlssOptionalTagPlan primaryPlan = BuildDlssOptionalTagPlan(primary);
    expect(primaryPlan.specularHitDistance && primaryPlan.responsivityMask
            && !primaryPlan.specularMotionVectors && !primaryPlan.disocclusionMask,
        "primary tag plan must contain only explicitly owned optional resources");

    DlssFrameInputs transmission{};
    transmission.useRayReconstruction = true;
    transmission.viewportId = primary.viewportId ^ 0x80000000u;
    transmission.disocclusionMask = reinterpret_cast<void*>(3);
    const DlssOptionalTagPlan transmissionPlan = BuildDlssOptionalTagPlan(transmission);
    expect(!transmissionPlan.specularHitDistance && !transmissionPlan.specularMotionVectors
            && !transmissionPlan.responsivityMask && transmissionPlan.disocclusionMask,
        "transmission cannot inherit primary optional guides");
    expect(primary.viewportId != transmission.viewportId
            && primary.responsivityMask != transmission.disocclusionMask,
        "primary and transmission viewport IDs and temporal masks must be independent");

    DlssFrameInputs invalid{};
    invalid.useRayReconstruction = true;
    invalid.responsivityMask = reinterpret_cast<void*>(4);
    invalid.disocclusionMask = reinterpret_cast<void*>(5);
    expect(!HasExclusiveRrTemporalMask(BuildDlssOptionalTagPlan(invalid)),
        "one evaluation must never submit responsivity and disocclusion together");
}
