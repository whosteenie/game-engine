#pragma once

#include "engine/Transform.h"

#include <glm/glm.hpp>

struct CameraComponent
{
    float fovDegrees = 45.0f;
    float nearPlane = 0.1f;
    float farPlane = 100.0f;
    bool enabled = true;
    int depth = 0;
    bool isMain = false;
};

CameraComponent MakeDefaultCameraComponent();
Transform MakeDefaultCameraTransform();
