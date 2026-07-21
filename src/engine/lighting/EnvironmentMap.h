#pragma once

#include "engine/lighting/EnvironmentIblSettings.h"

#include <cstdint>
#include <memory>
#include <string>

#include <glm/vec3.hpp>

class Camera;
class IBL;
class Shader;
class SkyboxRenderer;

enum class EnvironmentBackgroundMode
{
    Skybox = 0,
    SolidColor = 1,
};

// Scene-wide environment lighting: skybox background plus IBL cubemap / prefilter / irradiance.
// Reflection probes and SSR can supply alternate specular sources later without changing call sites.
class EnvironmentMap
{
public:
    EnvironmentMap();
    ~EnvironmentMap();

    EnvironmentMap(const EnvironmentMap&) = delete;
    EnvironmentMap& operator=(const EnvironmentMap&) = delete;
    EnvironmentMap(EnvironmentMap&& other) noexcept;
    EnvironmentMap& operator=(EnvironmentMap&& other) noexcept;

    EnvironmentBackgroundMode GetBackgroundMode() const { return m_backgroundMode; }
    void SetBackgroundMode(EnvironmentBackgroundMode mode);

    bool IsEnabled() const { return m_backgroundMode == EnvironmentBackgroundMode::Skybox; }
    void SetEnabled(const bool enabled);

    bool UsesSkyboxBackground() const;
    bool UsesSolidColorBackground() const;

    const std::string& GetHdrPath() const { return m_hdrPath; }
    void SetHdrPath(std::string path);

    float GetRotationDegrees() const { return m_rotationDegrees; }
    void SetRotationDegrees(float degrees);
    // Rebuild the baked cubemap/prefilter after interactive rotation editing has finished.
    void CommitRotation();
    float GetRotationYRadians() const;

    float GetExposure() const { return m_exposure; }
    void SetExposure(float exposure);

    glm::vec3 GetSolidBackgroundColorSrgb() const { return m_solidBackgroundColorSrgb; }
    void SetSolidBackgroundColorSrgb(const glm::vec3& colorSrgb);

    EnvironmentIblCubemapResolution GetIblCubemapResolution() const { return m_iblCubemapResolution; }
    void SetIblCubemapResolution(EnvironmentIblCubemapResolution resolution);

    bool IsLoaded() const;
    const std::string& GetLoadError() const;

    void SyncGpuResources();
    void ReloadSkyboxRenderer();
    void RenderSkybox(const Camera& camera, bool splitLightingMrt);

    IBL& GetIBL();
    const IBL& GetIBL() const;

private:
    void RequestReload();

    EnvironmentBackgroundMode m_backgroundMode = EnvironmentBackgroundMode::Skybox;
    std::string m_hdrPath;
    float m_rotationDegrees = 0.0f;
    float m_exposure = 1.0f;
    glm::vec3 m_solidBackgroundColorSrgb{0.08f, 0.09f, 0.15f};
    EnvironmentIblCubemapResolution m_iblCubemapResolution = EnvironmentIblCubemapResolution::Auto;

    bool m_reloadPending = true;
    std::string m_lastLoadedPath;
    float m_lastLoadedRotationDegrees = 0.0f;
    EnvironmentIblCubemapResolution m_lastLoadedIblCubemapResolution =
        EnvironmentIblCubemapResolution::Auto;

    std::unique_ptr<IBL> m_ibl;
    std::unique_ptr<SkyboxRenderer> m_skyboxRenderer;
};
