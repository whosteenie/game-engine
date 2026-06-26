#pragma once

#include <glm/glm.hpp>

// Per-frame camera matrices for motion-vector generation and temporal reprojection.
// Convention: velocity.xy = currentNDC.xy - previousNDC.xy (unjittered clip, perspective divide).
struct MotionVectorFrameState
{
    glm::mat4 prevView{1.0f};
    glm::mat4 prevProjection{1.0f};
    glm::mat4 prevUnjitteredProjection{1.0f};
    glm::mat4 prevViewProjection{1.0f};
    bool historyValid = false;
};
