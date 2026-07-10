#pragma once

#include "engine/rendering/MotionVectorFrameState.h"
#include "engine/rendering/post/PostProcessContext.h"
#include "engine/rendering/post/PostProcessTarget.h"

#include <cstdint>

#include <glm/glm.hpp>

class Framebuffer;
class Shader;

struct ScreenSpaceGiPassInputs
{
    bool runRadianceAssembly = false;

    bool ssgiEnabled = false;
    bool ssgiDenoiseEnabled = false;
    bool ssgiNoiseInjectionEnabled = false;

    bool sceneHasGeometryNormals = false;
    bool sceneHasMaterialGbuffer = false;

    glm::mat4 projectionMatrix{1.0f};
    glm::mat4 inverseProjectionMatrix{1.0f};
    glm::mat4 viewMatrix{1.0f};
    glm::mat4 unjitteredProjectionMatrix{1.0f};
    glm::vec2 texelSize{0.0f};

    MotionVectorFrameState motionVectorState{};

    int giFrameIndex = 0;
    bool radianceHistoryValid = false;

    std::uintptr_t radianceSrv = 0;
    std::uintptr_t depthSrv = 0;
    std::uintptr_t normalSrv = 0;
    std::uintptr_t material0Srv = 0;
    std::uintptr_t material1Srv = 0;

    float ssgiMaxTraceDistance = 100.0f;
    int ssgiStepCount = 32;
    float ssgiThickness = 0.5f;
    float ssgiNoiseStrength = 1.0f;
    float ssgiSpatialDepthThreshold = 0.01f;
    float ssgiSpatialBlurSpread = 1.0f;
    float ssgiRoughnessSpreadMin = 0.75f;
    float ssgiRoughnessSpreadMax = 1.75f;
    float giTemporalBlendFactor = 0.9f;
    float giDepthThreshold = 0.01f;

    Framebuffer* sceneFramebuffer = nullptr;

    Shader* ssgiTraceShader = nullptr;
    Shader* ssgiNoiseInjectShader = nullptr;
    Shader* ssgiDenoiseSpatialShader = nullptr;
    Shader* temporalReprojectShader = nullptr;
    Shader* giDepthHistoryShader = nullptr;

    PostProcessTarget* radianceTraceInputTarget = nullptr;
    PostProcessTarget* radianceSpatialTarget = nullptr;
    PostProcessTarget* radianceSpatialBlurTarget = nullptr;
    PostProcessTarget* radianceHistoryTarget = nullptr;
    PostProcessTarget* radianceTemporalTarget = nullptr;
    PostProcessTarget* radianceHistoryDepthTarget = nullptr;
};

struct ScreenSpaceGiPassOutputs
{
    std::uintptr_t lastSsgiInjectSrv = 0;

    bool runSsgiTrace = false;
    bool runSsgiDenoise = false;
    bool runGiTemporal = false;

    int giFrameIndex = 0;
    bool radianceHistoryValid = false;
};

class ScreenSpaceGiPass
{
public:
    static void Execute(
        const PostProcessContext& context,
        const ScreenSpaceGiPassInputs& inputs,
        ScreenSpaceGiPassOutputs& outputs);
};
