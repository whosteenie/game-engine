#include "engine/raytracing/core/PtRrGuideMath.h"
#include "engine/rendering/core/TemporalCameraPacket.h"
#include "test_expect.h"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace
{
    std::string ReadTextFile(const char* path)
    {
        std::ifstream file(path);
        if (!file.is_open())
        {
            throw std::runtime_error(std::string("Could not open source contract: ") + path);
        }
        std::ostringstream contents;
        contents << file.rdbuf();
        return contents.str();
    }

    std::string ReadFixtureFile()
    {
        try
        {
            return ReadTextFile("fixtures/pt_mirror_chain_hall.gameproject");
        }
        catch (const std::runtime_error&)
        {
            return ReadTextFile("tests/fixtures/pt_mirror_chain_hall.gameproject");
        }
    }
}

void RunPtMirrorChainGuideMathTests(int& failures)
{
    (void)failures;
    auto expectTrue = [](const bool condition, const char* message) {
        test::ExpectTrue(condition, message);
    };

    constexpr float kNear = 0.25f;
    constexpr float kFar = 100.0f;
    const glm::mat4 projection =
        glm::perspectiveLH_ZO(glm::radians(60.0f), 16.0f / 9.0f, kNear, kFar);
    const glm::mat4 currentView = glm::lookAtLH(
        glm::vec3(0.0f),
        glm::vec3(0.0f, 0.0f, 1.0f),
        glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 currentUnjittered = projection * currentView;
    const glm::mat4 currentJittered =
        TemporalCamera::ApplyJitter(projection, glm::vec2(0.015f, -0.02f)) * currentView;

    {
        const glm::mat4 previousView = glm::lookAtLH(
            glm::vec3(-1.0f, 0.0f, 0.0f),
            glm::vec3(-1.0f, 0.0f, 1.0f),
            glm::vec3(0.0f, 1.0f, 0.0f));
        const auto projected = PtRrGuideMath::ProjectStaticReceiver(
            glm::vec3(0.0f, 0.0f, 5.0f),
            currentJittered,
            currentUnjittered,
            projection * previousView,
            true);

        expectTrue(projected.valid, "Static mirror-chain receiver projection should be valid");
        expectTrue(
            projected.motionNdc.x < 0.0f,
            "Receiver motion uses current-minus-previous NDC sign during a rightward camera pan");
        test::ExpectNear(
            projected.motionNdc.x,
            projected.currentNdc.x - projected.previousNdc.x,
            1.0e-6f,
            "Receiver motion is current NDC minus previous NDC");
    }

    {
        const auto projected = PtRrGuideMath::ProjectStaticReceiver(
            glm::vec3(0.0f, 0.0f, 5.0f),
            currentJittered,
            currentUnjittered,
            currentUnjittered,
            false);

        expectTrue(projected.valid, "Receiver projection remains valid without temporal history");
        test::ExpectNear(projected.motionNdc.x, 0.0f, 1.0e-7f, "Invalid history clears receiver motion X");
        test::ExpectNear(projected.motionNdc.y, 0.0f, 1.0e-7f, "Invalid history clears receiver motion Y");
        test::ExpectNear(projected.currentNdc.x, 0.0f, 1.0e-6f, "Receiver motion projection is unjittered X");
        test::ExpectNear(projected.currentNdc.y, 0.0f, 1.0e-6f, "Receiver motion projection is unjittered Y");
    }

    {
        const auto nearProjection = PtRrGuideMath::ProjectStaticReceiver(
            glm::vec3(0.0f, 0.0f, kNear),
            currentUnjittered,
            currentUnjittered,
            currentUnjittered,
            true);
        const auto farProjection = PtRrGuideMath::ProjectStaticReceiver(
            glm::vec3(0.0f, 0.0f, kFar),
            currentUnjittered,
            currentUnjittered,
            currentUnjittered,
            true);

        expectTrue(nearProjection.valid && farProjection.valid, "LH-ZO near/far receiver projections are valid");
        test::ExpectNear(nearProjection.depth, 0.0f, 2.0e-6f, "D24 convention maps the near plane to zero");
        test::ExpectNear(farProjection.depth, 1.0f, 2.0e-6f, "D24 convention maps the far plane to one");

        const std::uint32_t quarter = PtRrGuideMath::EncodeD24Unorm(0.25f);
        expectTrue(quarter == 4'194'304u, "D24 UNORM uses all 24 depth bits with nearest rounding");
        test::ExpectNear(
            PtRrGuideMath::DecodeD24Unorm(quarter),
            0.25f,
            0.5f / static_cast<float>(PtRrGuideMath::kD24UnormMax),
            "D24 quarter-depth round-trip stays within half an UNORM step");
        test::ExpectNear(
            PtRrGuideMath::DecodeD24Unorm(quarter | 0xAB000000u),
            PtRrGuideMath::DecodeD24Unorm(quarter),
            0.0f,
            "D24 decode ignores the packed stencil byte");
        expectTrue(PtRrGuideMath::EncodeD24Unorm(-1.0f) == 0u, "D24 encoding clamps below zero");
        expectTrue(
            PtRrGuideMath::EncodeD24Unorm(2.0f) == PtRrGuideMath::kD24UnormMax,
            "D24 encoding clamps above one");
    }

    {
        // Canonical XZ chain used by the focused GPU fixture:
        // camera -> mirror A at the origin -> mirror B at (3,-3) -> receiver at (0,-6).
        // Unfolding B and then A puts the receiver on the camera's straight virtual ray.
        PtRrGuideMath::VirtualReflectionTransform transform{};
        const float angle = glm::radians(67.5f);
        expectTrue(
            transform.AppendReflection(
                glm::vec3(0.0f),
                glm::vec3(std::sin(angle), 0.0f, std::cos(angle))),
            "First planar mirror appends to the virtual transform");
        expectTrue(
            transform.AppendReflection(
                glm::vec3(3.0f, 0.0f, -3.0f),
                glm::vec3(1.0f, 0.0f, 0.0f)),
            "Second planar mirror appends to the virtual transform");

        const glm::vec3 physicalReceiver(0.0f, 0.0f, -6.0f);
        const glm::vec3 virtualReceiver = transform.ApplyPoint(physicalReceiver);
        test::ExpectNear(virtualReceiver.x, 0.0f, 2.0e-5f,
            "Two-mirror unfolding places the receiver on the center virtual ray");
        test::ExpectNear(virtualReceiver.z, -8.485281f, 2.0e-5f,
            "Two-mirror unfolding preserves the complete reflected path geometry");

        const glm::vec3 virtualNormal = glm::normalize(
            transform.ApplyDirection(glm::vec3(0.0f, 0.0f, 1.0f)));
        test::ExpectNear(virtualNormal.x, -0.7071068f, 2.0e-5f,
            "Receiver normal is unfolded through both mirror planes");
        test::ExpectNear(virtualNormal.z, 0.7071068f, 2.0e-5f,
            "Unfolded receiver normal retains unit orientation");

        const glm::mat4 chainProjection =
            glm::perspectiveLH_ZO(glm::radians(60.0f), 1.0f, kNear, kFar);
        const glm::mat4 chainCurrentView = glm::lookAtLH(
            glm::vec3(0.0f, 0.0f, 4.0f),
            glm::vec3(0.0f, 0.0f, 3.0f),
            glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat4 chainPreviousView = glm::lookAtLH(
            glm::vec3(0.5f, 0.0f, 4.0f),
            glm::vec3(0.5f, 0.0f, 3.0f),
            glm::vec3(0.0f, 1.0f, 0.0f));
        const auto virtualProjection = PtRrGuideMath::ProjectVirtualReceiver(
            transform,
            physicalReceiver,
            chainProjection * chainCurrentView,
            chainProjection * chainCurrentView,
            chainProjection * chainPreviousView,
            true);
        const auto directProjection = PtRrGuideMath::ProjectStaticReceiver(
            physicalReceiver,
            chainProjection * chainCurrentView,
            chainProjection * chainCurrentView,
            chainProjection * chainPreviousView,
            true);
        expectTrue(
            virtualProjection.valid && directProjection.valid,
            "Physical and unfolded receiver projections are finite");
        expectTrue(
            std::abs(virtualProjection.motionNdc.x - directProjection.motionNdc.x) > 0.005f,
            "Virtual motion is not the receiver's ordinary direct-view motion");
        expectTrue(
            virtualProjection.linearDepth > directProjection.linearDepth + 2.0f,
            "Virtual depth represents the unfolded path rather than physical receiver depth");
    }

    {
        PtRrGuideMath::GlossyConfidenceInputs confidenceInputs{};
        confidenceInputs.roughness = 0.08f;
        confidenceInputs.sampledPdf = 4.0f;
        confidenceInputs.coneWidthAtReceiver = 0.002f;
        confidenceInputs.scatterDistance = 3.0f;
        confidenceInputs.receiverLinearDepth = 5.0f;
        confidenceInputs.pixelSpreadAngle = 0.001f;
        const float baseline = PtRrGuideMath::ComputeGlossyChainConfidence(confidenceInputs);
        expectTrue(
            baseline >= 0.0f && baseline <= 1.0f,
            "Glossy mirror-chain confidence is normalized");

        auto rougher = confidenceInputs;
        rougher.roughness += 0.01f;
        expectTrue(
            PtRrGuideMath::ComputeGlossyChainConfidence(rougher) < baseline,
            "Glossy mirror-chain confidence decreases continuously with roughness");

        auto widerCone = confidenceInputs;
        widerCone.coneWidthAtReceiver *= 16.0f;
        expectTrue(
            PtRrGuideMath::ComputeGlossyChainConfidence(widerCone) < baseline,
            "Glossy mirror-chain confidence decreases as the ray-cone footprint widens");

        auto likelierSample = confidenceInputs;
        likelierSample.sampledPdf *= 2.0f;
        expectTrue(
            PtRrGuideMath::ComputeGlossyChainConfidence(likelierSample) > baseline,
            "Glossy mirror-chain confidence increases with sampled-lobe PDF");

        auto fartherSameWorldFootprint = confidenceInputs;
        fartherSameWorldFootprint.receiverLinearDepth *= 2.0f;
        expectTrue(
            PtRrGuideMath::ComputeGlossyChainConfidence(fartherSameWorldFootprint) > baseline,
            "A fixed world footprint gains confidence when it covers fewer distant pixels");

        auto infinitesimallyRougher = confidenceInputs;
        infinitesimallyRougher.roughness += 1.0e-5f;
        expectTrue(
            std::abs(
                PtRrGuideMath::ComputeGlossyChainConfidence(infinitesimallyRougher)
                - baseline) < 1.0e-3f,
            "Glossy mirror-chain confidence has no roughness threshold seam");
    }

    {
        const nlohmann::json fixture = nlohmann::json::parse(ReadFixtureFile());
        const nlohmann::json& scene = fixture.at("scene");
        const nlohmann::json& objects = scene.at("objects");
        int mirrorCount = 0;
        int orbitTargetCount = 0;
        for (const nlohmann::json& object : objects)
        {
            const std::string name = object.at("name").get<std::string>();
            if (name.rfind("Mirror ", 0) == 0)
            {
                ++mirrorCount;
                const nlohmann::json& material = object.at("material");
                expectTrue(
                    material.at("metallic").get<float>() == 1.0f
                        && material.at("roughness").get<float>() == 0.0f
                        && material.at("transmission").get<float>() == 0.0f,
                    "Hall fixture mirrors are opaque exact-delta metals");
            }
            if (name == "mirror-chain-orbit-target")
            {
                ++orbitTargetCount;
            }
        }

        const nlohmann::json& renderer = scene.at("renderer");
        expectTrue(mirrorCount == 3, "Hall fixture contains exactly three named mirror panels");
        expectTrue(orbitTargetCount == 1, "Hall fixture contains one deterministic orbit target");
        expectTrue(
            renderer.at("dxr").at("ptMaxBounces").get<int>() == 16,
            "Hall fixture is saved at the 16-bounce target");
        expectTrue(
            renderer.at("dxr").at("ptRrBundleMode").get<int>() == 0,
            "Hall fixture uses the complete PT RR guide bundle");
        expectTrue(
            renderer.at("screenSpaceEffects").at("dlssRayReconstruction").get<bool>(),
            "Hall fixture is saved with ray reconstruction enabled");
    }

    {
        PtRrGuideMath::InstanceBounds bounds{};
        bounds.minimum = glm::vec3(-0.5f, -0.25f, 5.0f);
        bounds.maximum = glm::vec3(0.5f, 0.25f, 5.1f);
        bounds.valid = true;
        const PtRrGuideMath::VirtualReflectionTransform identity{};
        const auto lowResolution = PtRrGuideMath::ProjectVirtualBounds(
            bounds, identity, currentUnjittered, glm::uvec2(640u, 360u));
        const auto highResolution = PtRrGuideMath::ProjectVirtualBounds(
            bounds, identity, currentUnjittered, glm::uvec2(1280u, 720u));
        expectTrue(lowResolution.valid && highResolution.valid,
            "PSR instance bounds project through the unjittered input camera");
        test::ExpectNear(
            highResolution.spanPixels,
            lowResolution.spanPixels * 2.0f,
            1.0e-3f,
            "Increasing RR input resolution delays sub-pixel PSR termination");

        auto invalidBounds = bounds;
        invalidBounds.valid = false;
        expectTrue(
            !PtRrGuideMath::ProjectVirtualBounds(
                invalidBounds, identity, currentUnjittered, glm::uvec2(640u, 360u)).valid,
            "Invalid bounds continue PSR instead of allowing early termination");

        auto nearCrossing = bounds;
        nearCrossing.minimum.z = -0.1f;
        nearCrossing.maximum.z = 0.1f;
        expectTrue(
            !PtRrGuideMath::ProjectVirtualBounds(
                nearCrossing, identity, currentUnjittered, glm::uvec2(640u, 360u)).valid,
            "Near-plane-crossing bounds remain conservative and cannot terminate early");

    }

    {
        expectTrue(
            PtRrGuideMath::kMirrorChainPsrFlag == 4u,
            "Mirror-chain PSR dispatch ABI uses the non-overlapping optical flag bit 2");

        const std::string shader = ReadTextFile(
            "assets/shaders/raytracing/path_tracing/path_tracer.hlsl");
        test::ExpectContains(
            shader,
            "g_PtOpticalStabilityFlagBits & 4u",
            "Shader mirror-chain gate matches the host optical flag bit");
        test::ExpectContains(
            shader,
            "&& kPtCenterPrimaryRays",
            "Shader mirror-chain gate remains real-time PT only");
        test::ExpectContains(
            shader,
            "#define g_MaxBounces g_SamplesPerPixel",
            "Mirror-chain feature preserves the established max-bounces dispatch ABI");
        test::ExpectContains(
            shader,
            "float ComputePtGlossyGuideConfidence(",
            "Shader keeps glossy chain confidence diagnostic explicit");
        test::ExpectContains(
            shader,
            "sampledPdf / (sampledPdf + 1.0)",
            "Shader glossy confidence retains the sampled-PDF term");
        test::ExpectContains(
            shader,
            "32.0 * alpha",
            "Shader glossy confidence retains the continuous slope term");
        test::ExpectContains(
            shader,
            "footprintPixels * footprintPixels",
            "Shader glossy confidence retains the projected-footprint term");
        const std::size_t samplerBegin = shader.find("void SampleOpaqueInterface(");
        const std::size_t samplerEnd = shader.find("bool SampleMaterialBounce(", samplerBegin);
        expectTrue(
            samplerBegin != std::string::npos && samplerEnd != std::string::npos,
            "Mirror-chain contract test locates the authoritative opaque sampler");
        if (samplerBegin != std::string::npos && samplerEnd != std::string::npos)
        {
            const std::string sampler = shader.substr(samplerBegin, samplerEnd - samplerBegin);
            expectTrue(
                sampler.find("OpaqueBsdfLobeSelectionProbFromNoV(") != std::string::npos,
                "Authoritative opaque sampler uses the shared zero-energy-lobe probability");
            expectTrue(
                sampler.find("clamp(pSpec") == std::string::npos,
                "Authoritative opaque sampler does not restore the stochastic perfect-metal ceiling");
        }
        test::ExpectContains(
            shader,
            "PtMirrorTransformPoint(virtualTransform, worldPosition)",
            "Mirror-chain owner unfolds receiver geometry before projection");
        test::ExpectContains(
            shader,
            "specHitDistGuide = g_MaxTraceDistance;",
            "Mirror-chain owner neutralizes the invalid folded hit-distance segment");
        test::ExpectContains(
            shader,
            "void ResolvePtPsrGBuffer(uint2 pixel)",
            "Mirror-chain receiver selection has a dedicated deterministic G-buffer pass");
        test::ExpectContains(
            shader,
            "void PathTracerPsrResolveRayGen()",
            "The deterministic receiver resolver has an independent ray-generation export");
        test::ExpectContains(
            shader,
            "void PathTracerShadeRayGen()",
            "Stochastic path shading has an independent ray-generation export");
        test::ExpectContains(
            shader,
            "bool resolvedPayloadPending = consumeResolvedPsr;",
            "The shading pass consumes the immutable resolved receiver payload");
        test::ExpectContains(
            shader,
            "record.transform2 = float4(",
            "The resolver exports the complete affine virtual-surface transform");
        test::ExpectContains(
            shader,
            "RWStructuredBuffer<uint4> g_PsrResolvedCurrent : register(u24);",
            "The resolver exports a compact persistent record instead of nine texture UAVs");

        const std::string opticalComposition = ReadTextFile(
            "assets/shaders/post/utility/pt_optical_layers.ps.hlsl");
        test::ExpectContains(
            opticalComposition,
            "transmission.rgb * lerp(1.0.xxx, throughput, owner)",
            "PSR glass preprocessing subtracts transmission in physical prefix space");
        test::ExpectContains(
            opticalComposition,
            "max(transmission.rgb, 0.0.xxx) * throughput",
            "PSR glass composition remultiplies reconstructed transmission by mirror throughput");
    }

    {
        expectTrue(
            !PtRrGuideMath::MayReplacePrimaryGuide(false, true, true, true, true),
            "Disabled mirror-chain guides preserve the primary guide for a valid virtual receiver");
        expectTrue(
            !PtRrGuideMath::MayReplacePrimaryGuide(true, true, false, true, true),
            "Invalid mirror-chain receiver preserves the primary guide");
        expectTrue(
            !PtRrGuideMath::MayReplacePrimaryGuide(true, true, true, false, true),
            "Non-virtual owner preserves the primary guide");
        expectTrue(
            !PtRrGuideMath::MayReplacePrimaryGuide(true, false, true, true, true),
            "Reference convergence preserves the primary guide even when the feature is enabled");
        expectTrue(
            !PtRrGuideMath::MayReplacePrimaryGuide(true, true, true, true, false),
            "Glossy sampled receivers preserve the primary guide without a validity interface");
        expectTrue(
            PtRrGuideMath::MayReplacePrimaryGuide(true, true, true, true, true),
            "Enabled valid virtual receiver may replace the primary guide");
    }
}
