#pragma once

#include "engine/rendering/post/PostProcessContext.h"
#include "engine/rendering/post/PostProcessTarget.h"

#include <cstdint>

#include <glm/glm.hpp>

class Camera;
class DirectionalShadowSettings;
class EnvironmentMap;
class Framebuffer;
class Shader;

struct ScreenCompositePrePassInputs
{
    bool pbrDebugActive = false;
    bool useShadowFactorComposite = false;

    glm::mat4 inverseProjectionMatrix{1.0f};
    glm::vec2 texelSize{0.0f};

    const DirectionalShadowSettings* shadowSettings = nullptr;
    Framebuffer* sceneFramebuffer = nullptr;

    std::uintptr_t shadowFactorSrv = 0;

    Shader* shadowBlurShader = nullptr;
    Shader* radianceAssemblyShader = nullptr;

    PostProcessTarget* shadowBlurTarget = nullptr;
    PostProcessTarget* shadowBlur2Target = nullptr;
    PostProcessTarget* radianceTarget = nullptr;
};

struct ScreenCompositePrePassOutputs
{
    std::uintptr_t shadowFactorSrv = 0;
    bool runRadianceAssembly = false;
};

struct ScreenCompositeDxrInputs
{
    bool pbrDebugActive = false;
    bool rtCompositeWanted = false;
    bool rtCompositeDebugOnly = false;
    bool rtHasFreshTrace = false;
    bool runRtGiInject = false;
    bool giHasFreshTrace = false;

    glm::mat4 inverseProjectionMatrix{1.0f};

    const Camera* camera = nullptr;
    const EnvironmentMap* environmentMap = nullptr;
    Framebuffer* sceneFramebuffer = nullptr;

    std::uintptr_t indirectCompositeSrv = 0;

    std::uintptr_t dxrReflectionSrv = 0;
    std::uintptr_t dxrReflectionDenoisedSrv = 0;
    float dxrReflectionMaxTraceDistance = 100.0f;
    float dxrReflectionRoughnessCutoff = 0.0f;
    float dxrReflectionUvScaleX = 1.0f;
    float dxrReflectionUvScaleY = 1.0f;

    std::uintptr_t giInjectSrv = 0;
    float dxrGiStrength = 1.0f;
    float dxrGiUvScaleX = 1.0f;
    float dxrGiUvScaleY = 1.0f;

    Shader* dxrIndirectShader = nullptr;
    Shader* dxrGiInjectShader = nullptr;

    PostProcessTarget* rtIndirectTarget = nullptr;
    PostProcessTarget* rtGiInjectTarget = nullptr;
};

struct ScreenCompositeDxrOutputs
{
    std::uintptr_t indirectCompositeSrv = 0;
    bool runRtIndirect = false;
};

struct ScreenCompositeHdrInputs
{
    bool pbrDebugActive = false;
    bool runAo = false;
    bool runGtao = false;
    bool useShadowFactorComposite = false;
    bool runRtGiInject = false;
    bool runSsgiTrace = false;

    const Camera* camera = nullptr;
    const EnvironmentMap* environmentMap = nullptr;
    Framebuffer* sceneFramebuffer = nullptr;

    std::uintptr_t aoCompositeSrv = 0;
    std::uintptr_t shadowFactorSrv = 0;
    std::uintptr_t indirectCompositeSrv = 0;
    std::uintptr_t lastSsgiInjectSrv = 0;

    float aoStrength = 1.0f;
    float ssaoPower = 1.0f;
    float gtaoPower = 1.0f;
    float ssgiStrength = 1.0f;

    std::uintptr_t dxrShadowDenoisedSrv = 0;
    float dxrShadowUvScaleX = 1.0f;
    float dxrShadowUvScaleY = 1.0f;
    bool dxrShadowCompositeEnabled = false;

    int debugMode = 0;

    Shader* compositeShader = nullptr;
    PostProcessTarget* radianceTarget = nullptr;
    PostProcessTarget* hdrCompositeTarget = nullptr;
};

struct ScreenCompositeHdrOutputs
{
    std::uintptr_t hdrColorSrv = 0;
    const char* hdrColorSource = "scene_direct";
    bool compositeRan = false;
};

class ScreenCompositePass
{
public:
    static void ExecutePreReflection(
        const PostProcessContext& context,
        const ScreenCompositePrePassInputs& inputs,
        ScreenCompositePrePassOutputs& outputs);

    static void ExecuteDxrIndirectChain(
        const PostProcessContext& context,
        const ScreenCompositeDxrInputs& inputs,
        ScreenCompositeDxrOutputs& outputs);

    static void ExecuteHdrComposite(
        const PostProcessContext& context,
        const ScreenCompositeHdrInputs& inputs,
        ScreenCompositeHdrOutputs& outputs);
};
