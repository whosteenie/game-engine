#include "engine/lighting/EnvironmentMap.h"

#include "engine/camera/Camera.h"
#include "engine/lighting/IBL.h"
#include "engine/rendering/core/Constants.h"
#include "engine/rendering/passes/SkyboxRenderer.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/vec3.hpp>

#include <cmath>
#include <filesystem>
#include <stdexcept>

namespace
{
    std::string UpgradeEnvironmentHdrPathIfAvailable(const std::string& path)
    {
        constexpr const char* kOneKSuffix = "_1k.hdr";
        constexpr std::size_t kSuffixLength = 7;
        if (path.size() < kSuffixLength
            || path.compare(path.size() - kSuffixLength, kSuffixLength, kOneKSuffix) != 0)
        {
            return path;
        }

        std::string upgradedPath = path.substr(0, path.size() - kSuffixLength);
        upgradedPath += "_2k.hdr";

        std::error_code error;
        if (std::filesystem::exists(upgradedPath, error))
        {
            return upgradedPath;
        }

        return path;
    }
}

EnvironmentMap::EnvironmentMap()
    : m_hdrPath(EngineConstants::EnvironmentHdr),
      m_ibl(std::make_unique<IBL>(m_hdrPath.c_str())),
      m_skyboxRenderer(std::make_unique<SkyboxRenderer>())
{
}

EnvironmentMap::~EnvironmentMap() = default;

EnvironmentMap::EnvironmentMap(EnvironmentMap&& other) noexcept = default;
EnvironmentMap& EnvironmentMap::operator=(EnvironmentMap&& other) noexcept = default;

void EnvironmentMap::SetBackgroundMode(const EnvironmentBackgroundMode mode)
{
    m_backgroundMode = mode;
}

void EnvironmentMap::SetEnabled(const bool enabled)
{
    m_backgroundMode =
        enabled ? EnvironmentBackgroundMode::Skybox : EnvironmentBackgroundMode::SolidColor;
}

bool EnvironmentMap::UsesSkyboxBackground() const
{
    return m_backgroundMode == EnvironmentBackgroundMode::Skybox && IsLoaded();
}

bool EnvironmentMap::UsesSolidColorBackground() const
{
    return m_backgroundMode == EnvironmentBackgroundMode::SolidColor;
}

void EnvironmentMap::SetHdrPath(std::string path)
{
    if (path.empty())
    {
        return;
    }

    path = UpgradeEnvironmentHdrPathIfAvailable(path);

    if (path != m_hdrPath)
    {
        m_hdrPath = std::move(path);
        RequestReload();
    }
}

void EnvironmentMap::SetRotationDegrees(const float degrees)
{
    if (std::abs(degrees - m_rotationDegrees) > 1e-4f)
    {
        m_rotationDegrees = degrees;
    }
}

void EnvironmentMap::CommitRotation()
{
    if (std::abs(m_rotationDegrees - m_lastLoadedRotationDegrees) > 1e-4f)
    {
        RequestReload();
    }
}

float EnvironmentMap::GetRotationYRadians() const
{
    return glm::radians(m_rotationDegrees);
}

void EnvironmentMap::SetExposure(const float exposure)
{
    m_exposure = exposure;
}

void EnvironmentMap::SetSolidBackgroundColorSrgb(const glm::vec3& colorSrgb)
{
    m_solidBackgroundColorSrgb = glm::clamp(colorSrgb, glm::vec3(0.0f), glm::vec3(1.0f));
}

void EnvironmentMap::SetIblCubemapResolution(const EnvironmentIblCubemapResolution resolution)
{
    if (resolution == m_iblCubemapResolution)
    {
        return;
    }

    m_iblCubemapResolution = resolution;
    if (m_ibl != nullptr && m_ibl->IsReady())
    {
        try
        {
            m_ibl->SetCubemapResolutionMode(resolution);
            m_lastLoadedIblCubemapResolution = resolution;
        }
        catch (...)
        {
            RequestReload();
        }
    }
    else
    {
        RequestReload();
    }
}

bool EnvironmentMap::IsLoaded() const
{
    return m_ibl != nullptr && m_ibl->IsReady();
}

const std::string& EnvironmentMap::GetLoadError() const
{
    static const std::string kEmptyError;
    if (m_ibl == nullptr)
    {
        return kEmptyError;
    }

    return m_ibl->GetLoadError();
}

void EnvironmentMap::RequestReload()
{
    m_reloadPending = true;
}

void EnvironmentMap::SyncGpuResources()
{
    if (m_ibl == nullptr)
    {
        return;
    }

    const bool sourceChanged = m_hdrPath != m_lastLoadedPath
        || m_iblCubemapResolution != m_lastLoadedIblCubemapResolution;

    if (!m_reloadPending && !sourceChanged && m_ibl->IsReady())
    {
        return;
    }

    m_reloadPending = false;
    const float rotationYRadians = glm::radians(m_rotationDegrees);
    m_ibl->SetCubemapResolutionMode(m_iblCubemapResolution);

    std::string hdrPath = UpgradeEnvironmentHdrPathIfAvailable(m_hdrPath);
    if (hdrPath != m_hdrPath)
    {
        m_hdrPath = hdrPath;
    }

    try
    {
        if (m_ibl->IsReady() && hdrPath == m_ibl->GetHdrPath()
            && std::abs(rotationYRadians - m_ibl->GetRotationYRadians()) < 1e-6f
            && m_iblCubemapResolution == m_lastLoadedIblCubemapResolution)
        {
            m_lastLoadedPath = m_hdrPath;
            m_lastLoadedRotationDegrees = m_rotationDegrees;
            return;
        }

        m_ibl->ReloadFromHdr(hdrPath.c_str(), rotationYRadians);
        m_ibl->SetCubemapResolutionMode(m_iblCubemapResolution);
        m_lastLoadedPath = hdrPath;
        m_lastLoadedRotationDegrees = m_rotationDegrees;
        m_lastLoadedIblCubemapResolution = m_iblCubemapResolution;
    }
    catch (...)
    {
        m_lastLoadedPath.clear();
        m_lastLoadedRotationDegrees = 0.0f;
    }
}

void EnvironmentMap::ReloadSkyboxRenderer()
{
    m_skyboxRenderer = std::make_unique<SkyboxRenderer>();
}

void EnvironmentMap::RenderSkybox(const Camera& camera, const bool splitLightingMrt)
{
    if (!UsesSkyboxBackground() || m_skyboxRenderer == nullptr)
    {
        return;
    }

    m_skyboxRenderer->Draw(camera, *m_ibl, m_exposure, splitLightingMrt);
}

IBL& EnvironmentMap::GetIBL()
{
    if (m_ibl == nullptr)
    {
        throw std::runtime_error("Environment map IBL is not initialized");
    }

    return *m_ibl;
}

const IBL& EnvironmentMap::GetIBL() const
{
    if (m_ibl == nullptr)
    {
        throw std::runtime_error("Environment map IBL is not initialized");
    }

    return *m_ibl;
}
