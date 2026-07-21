#pragma once

#include "engine/rendering/core/TemporalCameraPacket.h"

#include <glm/glm.hpp>

// Per-frame camera matrices for motion-vector generation and temporal reprojection.
// Convention: velocity.xy = currentNDC.xy - previousNDC.xy (unjittered clip, perspective divide).
struct MotionVectorFrameState
{
    glm::mat4 prevView{1.0f};
    glm::mat4 prevProjection{1.0f};
    glm::mat4 prevUnjitteredProjection{1.0f};
    glm::mat4 prevViewProjection{1.0f};
    // Complete camera state for the previous compatible rendered frame. The matrix fields above
    // remain for raster consumers; SceneRenderer asserts that these retained duplicates agree.
    TemporalCameraState previousCamera{};
    bool historyValid = false;
};
