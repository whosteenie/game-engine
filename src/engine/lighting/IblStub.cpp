#include "engine/lighting/IBL.h"

#include "engine/rendering/Shader.h"

IBL::IBL(const char* /*hdrPath*/)
{
}

IBL::~IBL()
{
    DestroyResources();
}

IBL::IBL(IBL&& other) noexcept
    : m_environmentIntensity(other.m_environmentIntensity)
{
    other.m_environmentIntensity = 0.4f;
}

IBL& IBL::operator=(IBL&& other) noexcept
{
    if (this != &other)
    {
        DestroyResources();
        m_environmentIntensity = other.m_environmentIntensity;
        other.m_environmentIntensity = 0.4f;
    }

    return *this;
}

void IBL::DestroyResources()
{
}

void IBL::CreateCaptureResources()
{
}

void IBL::LoadHdrEquirectangular(const char* /*hdrPath*/)
{
}

void IBL::CreateEnvironmentCubemap()
{
}

void IBL::CreateIrradianceMap()
{
}

void IBL::CreatePrefilterMap()
{
}

void IBL::CreateBrdfLut()
{
}

void IBL::CaptureCubemapFaces(
    unsigned int /*targetCubemap*/,
    Shader& /*shader*/,
    unsigned int /*resolution*/,
    unsigned int /*mipLevel*/,
    bool /*generateMipmapsAfter*/)
{
}

void IBL::BindTextures(Shader& /*shader*/) const
{
}

float IBL::GetMaxReflectionLod() const
{
    return m_maxPrefilterMipLevel;
}

float IBL::GetEnvironmentIntensity() const
{
    return m_environmentIntensity;
}

void IBL::SetEnvironmentIntensity(float intensity)
{
    m_environmentIntensity = intensity;
}
