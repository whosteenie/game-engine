#pragma once

#include "engine/rendering/post/PostProcessContext.h"
#include "engine/rendering/post/PostProcessTarget.h"

#include <cstdint>

#include <glm/glm.hpp>

class Shader;

struct AmbientOcclusionPassInputs
{
    bool runSsao = false;
    bool runGtao = false;

    glm::mat4 projectionMatrix{1.0f};
    glm::mat4 inverseProjectionMatrix{1.0f};
    glm::mat4 viewMatrix{1.0f};
    glm::vec2 texelSize{0.0f};

    float nearPlane = 0.1f;
    float farPlane = 10000.0f;
    bool hasGeometryNormals = false;

    std::uintptr_t depthSrv = 0;
    std::uintptr_t normalSrv = 0;
    std::uintptr_t noiseSrv = 0;

    float ssaoRadius = 0.6f;
    float ssaoBias = 0.025f;
    int ssaoShaderDebugMode = 0;
    const glm::vec3* kernelSamples = nullptr;
    int kernelSampleCount = 0;

    float gtaoRadius = 1.0f;
    float gtaoThickness = 1.0f;
    float gtaoFalloff = 1.0f;
    int gtaoDirections = 4;
    int gtaoSteps = 4;
    bool gtaoDenoiseEnabled = true;

    float ssaoBlurDepthThreshold = 0.01f;

    Shader* ssaoShader = nullptr;
    Shader* gtaoShader = nullptr;
    Shader* blurShader = nullptr;

    PostProcessTarget* ssaoTarget = nullptr;
    PostProcessTarget* ssaoBlurTarget = nullptr;
    PostProcessTarget* gtaoRawTarget = nullptr;
};

struct AmbientOcclusionPassOutputs
{
    std::uintptr_t aoCompositeSrv = 0;
};

class AmbientOcclusionPass
{
public:
    static void Execute(
        const PostProcessContext& context,
        const AmbientOcclusionPassInputs& inputs,
        AmbientOcclusionPassOutputs& outputs);
};
