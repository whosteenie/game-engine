#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <glm/glm.hpp>

// Authoritative per-viewport camera history used by temporal rendering consumers.
//
// Matrices use GLM column-major storage and the renderer's left-handed, zero-to-one clip space.
// projection is deliberately unjittered. jitterNdc is the NDC offset applied to projection[2].xy;
// keeping it separate lets consumers select their sampling convention without losing the exact
// rendered-frame jitter. inverseViewProjection is inverse(projection * view), also unjittered.
// worldPosition is in renderer world space. A state is usable only when valid and complete.
struct TemporalCameraState
{
    glm::mat4 view{0.0f};
    glm::mat4 projection{0.0f};
    glm::mat4 inverseViewProjection{0.0f};
    glm::vec3 worldPosition{0.0f};
    glm::vec2 jitterNdc{0.0f};
    bool valid = false;
};

struct TemporalCameraPacket
{
    TemporalCameraState current{};
    TemporalCameraState previous{};
};

namespace TemporalCamera
{
inline bool IsFinite(const glm::mat4& value)
{
    for (std::size_t column = 0; column < 4; ++column)
    {
        for (std::size_t row = 0; row < 4; ++row)
        {
            if (!std::isfinite(value[column][row]))
            {
                return false;
            }
        }
    }
    return true;
}

inline bool IsFinite(const glm::vec3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

inline bool IsFinite(const glm::vec2& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y);
}

inline bool NearlyEqual(const float lhs, const float rhs, const float epsilon = 1.0e-4f)
{
    const float scale = std::max(1.0f, std::max(std::abs(lhs), std::abs(rhs)));
    return std::abs(lhs - rhs) <= epsilon * scale;
}

inline bool NearlyEqual(
    const glm::mat4& lhs,
    const glm::mat4& rhs,
    const float epsilon = 1.0e-4f)
{
    for (std::size_t column = 0; column < 4; ++column)
    {
        for (std::size_t row = 0; row < 4; ++row)
        {
            if (!NearlyEqual(lhs[column][row], rhs[column][row], epsilon))
            {
                return false;
            }
        }
    }
    return true;
}

inline bool NearlyEqual(
    const glm::vec3& lhs,
    const glm::vec3& rhs,
    const float epsilon = 1.0e-4f)
{
    return NearlyEqual(lhs.x, rhs.x, epsilon)
        && NearlyEqual(lhs.y, rhs.y, epsilon)
        && NearlyEqual(lhs.z, rhs.z, epsilon);
}

inline bool NearlyEqual(
    const glm::vec2& lhs,
    const glm::vec2& rhs,
    const float epsilon = 1.0e-4f)
{
    return NearlyEqual(lhs.x, rhs.x, epsilon) && NearlyEqual(lhs.y, rhs.y, epsilon);
}

inline bool IsComplete(const TemporalCameraState& state)
{
    if (!state.valid || !IsFinite(state.view) || !IsFinite(state.projection)
        || !IsFinite(state.inverseViewProjection) || !IsFinite(state.worldPosition)
        || !IsFinite(state.jitterNdc))
    {
        return false;
    }

    const glm::mat4 viewProjection = state.projection * state.view;
    const glm::mat4 identity = viewProjection * state.inverseViewProjection;
    if (!NearlyEqual(identity, glm::mat4(1.0f), 5.0e-4f))
    {
        return false;
    }

    const glm::vec3 positionFromView = glm::vec3(glm::inverse(state.view)[3]);
    return IsFinite(positionFromView)
        && NearlyEqual(positionFromView, state.worldPosition, 5.0e-4f);
}

inline TemporalCameraState MakeState(
    const glm::mat4& view,
    const glm::mat4& unjitteredProjection,
    const glm::mat4& inverseUnjitteredViewProjection,
    const glm::vec3& worldPosition,
    const glm::vec2& jitterNdc,
    const bool available = true)
{
    TemporalCameraState state{};
    state.view = view;
    state.projection = unjitteredProjection;
    state.inverseViewProjection = inverseUnjitteredViewProjection;
    state.worldPosition = worldPosition;
    state.jitterNdc = jitterNdc;
    state.valid = available;
    state.valid = IsComplete(state);
    return state;
}

inline glm::mat4 ApplyJitter(const glm::mat4& unjitteredProjection, const glm::vec2& jitterNdc)
{
    glm::mat4 projection = unjitteredProjection;
    projection[2][0] += jitterNdc.x;
    projection[2][1] += jitterNdc.y;
    return projection;
}

inline bool Agree(
    const TemporalCameraState& lhs,
    const TemporalCameraState& rhs,
    const float epsilon = 1.0e-4f)
{
    return lhs.valid == rhs.valid
        && NearlyEqual(lhs.view, rhs.view, epsilon)
        && NearlyEqual(lhs.projection, rhs.projection, epsilon)
        && NearlyEqual(lhs.inverseViewProjection, rhs.inverseViewProjection, epsilon)
        && NearlyEqual(lhs.worldPosition, rhs.worldPosition, epsilon)
        && NearlyEqual(lhs.jitterNdc, rhs.jitterNdc, epsilon);
}
}
