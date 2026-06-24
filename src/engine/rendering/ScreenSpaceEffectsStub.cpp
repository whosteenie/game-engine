#include "engine/rendering/ScreenSpaceEffects.h"

#include "engine/camera/Camera.h"
#include "engine/lighting/DirectionalShadowSettings.h"
#include "engine/rendering/Framebuffer.h"
#include "engine/rendering/Shader.h"

ScreenSpaceEffects::ScreenSpaceEffects() = default;

ScreenSpaceEffects::~ScreenSpaceEffects() = default;

void ScreenSpaceEffects::Resize(int /*width*/, int /*height*/)
{
}

void ScreenSpaceEffects::BeginScenePass() const
{
}

void ScreenSpaceEffects::EndScenePass() const
{
}

void ScreenSpaceEffects::Apply(
    const Camera& /*camera*/,
    int /*viewportWidth*/,
    int /*viewportHeight*/,
    const DirectionalShadowSettings& /*shadowSettings*/) const
{
}

bool ScreenSpaceEffects::IsEnabled() const
{
    return m_enabled;
}

void ScreenSpaceEffects::SetEnabled(bool enabled)
{
    m_enabled = enabled;
}

bool ScreenSpaceEffects::IsSsaoEnabled() const
{
    return m_ssaoEnabled;
}

void ScreenSpaceEffects::SetSsaoEnabled(bool enabled)
{
    m_ssaoEnabled = enabled;
}

float ScreenSpaceEffects::GetSsaoRadius() const
{
    return m_ssaoRadius;
}

void ScreenSpaceEffects::SetSsaoRadius(float radius)
{
    m_ssaoRadius = radius;
}

float ScreenSpaceEffects::GetSsaoBias() const
{
    return m_ssaoBias;
}

void ScreenSpaceEffects::SetSsaoBias(float bias)
{
    m_ssaoBias = bias;
}

float ScreenSpaceEffects::GetSsaoPower() const
{
    return m_ssaoPower;
}

void ScreenSpaceEffects::SetSsaoPower(float power)
{
    m_ssaoPower = power;
}

float ScreenSpaceEffects::GetAoStrength() const
{
    return m_aoStrength;
}

void ScreenSpaceEffects::SetAoStrength(float strength)
{
    m_aoStrength = strength;
}

float ScreenSpaceEffects::GetExposure() const
{
    return m_exposure;
}

void ScreenSpaceEffects::SetExposure(float exposure)
{
    m_exposure = exposure;
}

TonemapMode ScreenSpaceEffects::GetTonemapMode() const
{
    return m_tonemapMode;
}

void ScreenSpaceEffects::SetTonemapMode(TonemapMode mode)
{
    m_tonemapMode = mode;
}

bool ScreenSpaceEffects::IsBloomEnabled() const
{
    return m_bloomEnabled;
}

void ScreenSpaceEffects::SetBloomEnabled(bool enabled)
{
    m_bloomEnabled = enabled;
}

float ScreenSpaceEffects::GetBloomThreshold() const
{
    return m_bloomThreshold;
}

void ScreenSpaceEffects::SetBloomThreshold(float threshold)
{
    m_bloomThreshold = threshold;
}

float ScreenSpaceEffects::GetBloomSoftKnee() const
{
    return m_bloomSoftKnee;
}

void ScreenSpaceEffects::SetBloomSoftKnee(float softKnee)
{
    m_bloomSoftKnee = softKnee;
}

float ScreenSpaceEffects::GetBloomIntensity() const
{
    return m_bloomIntensity;
}

void ScreenSpaceEffects::SetBloomIntensity(float intensity)
{
    m_bloomIntensity = intensity;
}

float ScreenSpaceEffects::GetBloomBlurRadius() const
{
    return m_bloomBlurRadius;
}

void ScreenSpaceEffects::SetBloomBlurRadius(float blurRadius)
{
    m_bloomBlurRadius = blurRadius;
}

RenderDebugMode ScreenSpaceEffects::GetDebugMode() const
{
    return m_debugMode;
}

void ScreenSpaceEffects::SetDebugMode(RenderDebugMode mode)
{
    m_debugMode = mode;
}

void ScreenSpaceEffects::BlitDepthToFramebuffer(
    std::uintptr_t /*drawFramebuffer*/,
    int /*viewportWidth*/,
    int /*viewportHeight*/) const
{
}
