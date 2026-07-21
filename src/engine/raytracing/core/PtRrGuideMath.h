#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

// CPU-side contract helpers for path-traced RR receiver guides. These are intentionally independent
// of a TLAS and are suitable for validation/capture tooling. Matrices follow the renderer convention:
// GLM column-major storage, left-handed zero-to-one clip depth, jittered depth, and unjittered motion.
namespace PtRrGuideMath
{
constexpr std::uint32_t kD24UnormMax = (1u << 24u) - 1u;
constexpr std::uint32_t kOpticalMotionReplayFlag = 1u << 0u;
constexpr std::uint32_t kIndependentOpticalRrFlag = 1u << 1u;
constexpr std::uint32_t kMirrorChainPsrFlag = 1u << 2u;
static_assert(
    (kMirrorChainPsrFlag & (kOpticalMotionReplayFlag | kIndependentOpticalRrFlag)) == 0u,
    "Mirror-chain PSR flag must not overlap existing PT optical flags");

enum class PsrTerminalReason : std::uint32_t
{
    PrimaryReceiver = 0,
    PsrReceiver = 1,
    EnvironmentEscape = 2,
    IneligibleLinkFallback = 3,
    SubpixelTail = 4,
    NegligibleThroughputTail = 5,
    HardCapSignificant = 6,
    InvalidProjectionFallback = 7,
};

struct InstanceBounds
{
    glm::vec3 minimum{0.0f};
    glm::vec3 maximum{0.0f};
    bool valid = false;
};

struct ProjectedBounds
{
    bool valid = false;
    glm::vec2 minimumPixels{0.0f};
    glm::vec2 maximumPixels{0.0f};
    float spanPixels = 0.0f;
};

struct ReceiverProjection
{
    bool valid = false;
    glm::vec2 currentNdc{0.0f};
    glm::vec2 previousNdc{0.0f};
    glm::vec2 motionNdc{0.0f};
    float depth = 1.0f;
    float linearDepth = 0.0f;
    float previousLinearDepth = 0.0f;
    float previousDepthDelta = 0.0f;
};

// Affine unfolding transform for a sequence of static planar mirror reflections. Mirrors are
// appended in camera-to-receiver order; applying the result to a physical receiver maps it into
// the virtual world seen through the complete chain.
struct VirtualReflectionTransform
{
    glm::mat3 linear{1.0f};
    glm::vec3 translation{0.0f};
    bool valid = true;

    bool AppendReflection(const glm::vec3& planePoint, const glm::vec3& planeNormal)
    {
        const float normalLengthSquared = glm::dot(planeNormal, planeNormal);
        if (!std::isfinite(normalLengthSquared) || normalLengthSquared <= 1.0e-12f)
        {
            valid = false;
            return false;
        }

        const glm::vec3 normal = planeNormal / std::sqrt(normalLengthSquared);
        const glm::mat3 reflectionLinear = glm::mat3(1.0f)
            - 2.0f * glm::outerProduct(normal, normal);
        const glm::vec3 reflectionTranslation =
            2.0f * glm::dot(normal, planePoint) * normal;
        const glm::mat3 previousLinear = linear;
        linear = previousLinear * reflectionLinear;
        translation += previousLinear * reflectionTranslation;
        valid = std::isfinite(translation.x) && std::isfinite(translation.y)
            && std::isfinite(translation.z);
        for (int column = 0; column < 3 && valid; ++column)
        {
            valid = std::isfinite(linear[column].x)
                && std::isfinite(linear[column].y)
                && std::isfinite(linear[column].z);
        }
        return valid;
    }

    glm::vec3 ApplyPoint(const glm::vec3& point) const
    {
        return linear * point + translation;
    }

    glm::vec3 ApplyDirection(const glm::vec3& direction) const
    {
        return linear * direction;
    }
};

struct GlossyConfidenceInputs
{
    float roughness = 0.0f;
    float sampledPdf = 0.0f;
    float coneWidthAtReceiver = 0.0f;
    float scatterDistance = 0.0f;
    float receiverLinearDepth = 0.0f;
    float pixelSpreadAngle = 0.0f;
};

inline bool IsFinite(const glm::vec4& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y)
        && std::isfinite(value.z) && std::isfinite(value.w);
}

// Conservative PSR stop metric. A corner at/behind the near clip plane invalidates the estimate so
// the resolver continues to its hard cap; it must never terminate a potentially visible mirror
// early. The transform represents only mirrors preceding the bounds-owning link.
inline ProjectedBounds ProjectVirtualBounds(
    const InstanceBounds& bounds,
    const VirtualReflectionTransform& precedingMirrors,
    const glm::mat4& unjitteredViewProjection,
    const glm::uvec2 rrInputExtent)
{
    ProjectedBounds result{};
    if (!bounds.valid || !precedingMirrors.valid || rrInputExtent.x == 0u || rrInputExtent.y == 0u)
    {
        return result;
    }

    glm::vec2 minimumNdc(std::numeric_limits<float>::max());
    glm::vec2 maximumNdc(std::numeric_limits<float>::lowest());
    for (std::uint32_t corner = 0; corner < 8u; ++corner)
    {
        const glm::vec3 physical(
            (corner & 1u) != 0u ? bounds.maximum.x : bounds.minimum.x,
            (corner & 2u) != 0u ? bounds.maximum.y : bounds.minimum.y,
            (corner & 4u) != 0u ? bounds.maximum.z : bounds.minimum.z);
        const glm::vec4 clip = unjitteredViewProjection
            * glm::vec4(precedingMirrors.ApplyPoint(physical), 1.0f);
        if (!IsFinite(clip) || clip.w <= 1.0e-6f)
        {
            return result;
        }
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        minimumNdc = glm::min(minimumNdc, ndc);
        maximumNdc = glm::max(maximumNdc, ndc);
    }

    // Clipping the rectangle to the screen is conservative for the visible footprint once every
    // corner is safely in front of the camera.
    minimumNdc = glm::clamp(minimumNdc, glm::vec2(-1.0f), glm::vec2(1.0f));
    maximumNdc = glm::clamp(maximumNdc, glm::vec2(-1.0f), glm::vec2(1.0f));
    if (maximumNdc.x < minimumNdc.x || maximumNdc.y < minimumNdc.y)
    {
        return result;
    }

    const glm::vec2 extent(rrInputExtent);
    result.minimumPixels = (minimumNdc * 0.5f + 0.5f) * extent;
    result.maximumPixels = (maximumNdc * 0.5f + 0.5f) * extent;
    const glm::vec2 size = glm::max(result.maximumPixels - result.minimumPixels, glm::vec2(0.0f));
    result.spanPixels = std::max(size.x, size.y);
    result.valid = std::isfinite(result.spanPixels);
    return result;
}

inline ReceiverProjection ProjectStaticReceiver(
    const glm::vec3& worldPosition,
    const glm::mat4& currentJitteredViewProjection,
    const glm::mat4& currentUnjitteredViewProjection,
    const glm::mat4& previousUnjitteredViewProjection,
    const bool motionHistoryValid)
{
    const glm::vec4 world(worldPosition, 1.0f);
    const glm::vec4 currentClipJittered = currentJitteredViewProjection * world;
    const glm::vec4 currentClipUnjittered = currentUnjitteredViewProjection * world;
    glm::vec4 previousClipUnjittered = previousUnjitteredViewProjection * world;
    if (!motionHistoryValid)
    {
        previousClipUnjittered = currentClipUnjittered;
    }

    ReceiverProjection result{};
    constexpr float kMinClipW = 1.0e-6f;
    if (!IsFinite(currentClipJittered) || !IsFinite(currentClipUnjittered)
        || !IsFinite(previousClipUnjittered)
        || currentClipJittered.w <= kMinClipW
        || currentClipUnjittered.w <= kMinClipW
        || previousClipUnjittered.w <= kMinClipW)
    {
        return result;
    }

    result.currentNdc = glm::vec2(currentClipUnjittered) / currentClipUnjittered.w;
    result.previousNdc = glm::vec2(previousClipUnjittered) / previousClipUnjittered.w;
    result.motionNdc = result.currentNdc - result.previousNdc;
    result.depth = std::clamp(currentClipJittered.z / currentClipJittered.w, 0.0f, 1.0f);
    result.linearDepth = currentClipUnjittered.w;
    result.previousLinearDepth = previousClipUnjittered.w;
    result.previousDepthDelta = result.previousLinearDepth - result.linearDepth;
    result.valid = std::isfinite(result.currentNdc.x) && std::isfinite(result.currentNdc.y)
        && std::isfinite(result.previousNdc.x) && std::isfinite(result.previousNdc.y)
        && std::isfinite(result.depth) && std::isfinite(result.previousDepthDelta);
    return result;
}

inline ReceiverProjection ProjectVirtualReceiver(
    const VirtualReflectionTransform& transform,
    const glm::vec3& physicalWorldPosition,
    const glm::mat4& currentJitteredViewProjection,
    const glm::mat4& currentUnjitteredViewProjection,
    const glm::mat4& previousUnjitteredViewProjection,
    const bool motionHistoryValid)
{
    if (!transform.valid)
    {
        return {};
    }
    return ProjectStaticReceiver(
        transform.ApplyPoint(physicalWorldPosition),
        currentJitteredViewProjection,
        currentUnjitteredViewProjection,
        previousUnjitteredViewProjection,
        motionHistoryValid);
}

inline std::uint32_t EncodeD24Unorm(const float depth)
{
    const float normalized = std::clamp(depth, 0.0f, 1.0f);
    return static_cast<std::uint32_t>(
        std::lround(normalized * static_cast<float>(kD24UnormMax)));
}

inline float DecodeD24Unorm(const std::uint32_t encoded)
{
    return static_cast<float>(encoded & kD24UnormMax)
        / static_cast<float>(kD24UnormMax);
}

// Diagnostic-only confidence for an actual non-delta specular event. RR has no safe per-pixel
// validity input, so this value must not select a glossy receiver bundle; production glossy paths
// retain the primary guide. The terms express sampled-lobe likelihood and projected lobe footprint
// without introducing another material roughness cutoff.
inline float ComputeGlossyChainConfidence(const GlossyConfidenceInputs& inputs)
{
    const float alpha = std::max(inputs.roughness * inputs.roughness, 1.0e-3f);
    const float scatterDistance = std::max(inputs.scatterDistance, 0.0f);
    const float lobeFootprintWorld = std::max(
        inputs.coneWidthAtReceiver,
        alpha * scatterDistance);
    const float pixelFootprintWorld = std::max(
        inputs.receiverLinearDepth * inputs.pixelSpreadAngle,
        1.0e-6f);
    const float footprintPixels = lobeFootprintWorld / pixelFootprintWorld;
    const float pdfConfidence = inputs.sampledPdf / (inputs.sampledPdf + 1.0f);
    const float slopeConfidence = 1.0f / (1.0f + 32.0f * alpha);
    const float footprintConfidence = 1.0f / (1.0f + footprintPixels * footprintPixels);
    return std::clamp(pdfConfidence * slopeConfidence * footprintConfidence, 0.0f, 1.0f);
}

// The persisted feature flag is the first and unconditional gate; only a real-time exact-delta
// virtual receiver can pass the remaining policy. Keeping this helper separate makes disabled and
// glossy fallback equivalence testable without reproducing path-shader state.
inline bool MayReplacePrimaryGuide(
    const bool featureEnabled,
    const bool centerPrimaryRays,
    const bool receiverValid,
    const bool virtualReceiver,
    const bool exactDeltaChain)
{
    return featureEnabled && centerPrimaryRays && receiverValid && virtualReceiver
        && exactDeltaChain;
}
}
