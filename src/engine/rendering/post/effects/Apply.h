#pragma once

#include "engine/lighting/DirectionalShadowSettings.h"
#include "engine/lighting/EnvironmentMap.h"
#include "engine/rendering/resources/Framebuffer.h"
#include "engine/rendering/core/RenderDebug.h"
#include "engine/rendering/post/ScreenSpaceEffects.h"
#include "engine/rendering/post/AmbientOcclusionPass.h"
#include "engine/rendering/post/DlssResolvePass.h"
#include "engine/rendering/post/ScreenSpaceGiPass.h"
#include "engine/rendering/post/ScreenSpaceReflectionPass.h"

#include <cstdint>

#include <glm/glm.hpp>

class Camera;

// Per-frame state for ScreenSpaceEffects::Apply (HK-C9).
struct ScreenSpaceEffects::ApplyFrameState
{
    const Camera* camera = nullptr;
    const DirectionalShadowSettings* shadowSettings = nullptr;
    const EnvironmentMap* environmentMap = nullptr;
    const Framebuffer* outputTarget = nullptr;
    int viewportWidth = 0;
    int viewportHeight = 0;

    glm::mat4 projectionMatrix{1.0f};
    glm::mat4 inverseProjectionMatrix{1.0f};
    glm::vec2 texelSize{0.0f};

    RenderDebugMode debugMode = RenderDebugMode::None;
    bool pbrDebugActive = false;
    bool runAo = false;
    bool runSsao = false;
    bool runGtao = false;
    bool useShadowFactorComposite = false;
    bool runRadianceAssembly = false;
    bool rtCompositeWanted = false;
    bool rtCompositeDebugOnly = false;
    bool rtHasFreshTrace = false;
    bool runRtIndirect = false;
    bool runRtGiInject = false;
    bool giHasFreshTrace = false;
    bool runSsrIndirect = false;
    bool runSsgiTrace = false;
    bool compositeRan = false;
    bool compositeUsesSsao = false;
    bool useTaa = false;
    bool wantDlss = false;
    bool pathTracerReferenceActive = false;
    bool effectiveWantDlss = false;

    std::uintptr_t aoCompositeSrv = 0;
    std::uintptr_t shadowFactorSrv = 0;
    std::uintptr_t indirectCompositeSrv = 0;
    std::uintptr_t giInjectSrv = 0;
    std::uintptr_t hdrColorSrv = 0;
    std::uintptr_t bloomSrv = 0;

    const char* hdrColorSource = "scene_direct";
    const char* ssaoDebugViewSource = "none";

    AmbientOcclusionPassInputs aoInputs{};
    ScreenSpaceReflectionPassInputs ssrInputs{};
    ScreenSpaceGiPassInputs ssgiInputs{};
    DlssResolvePassInputs dlssInputs{};
};
