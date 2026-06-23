#include "engine/lighting/IBL.h"

#include "engine/rendering/Constants.h"
#include "engine/rendering/Shader.h"

#include <glad/glad.h>

#include <stb_image.h>

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <stdexcept>
#include <string>

namespace
{
    const float kCubeVertices[] = {
        -1.0f,  1.0f, -1.0f,
        -1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,

        -1.0f, -1.0f,  1.0f,
        -1.0f, -1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f,  1.0f,
        -1.0f, -1.0f,  1.0f,

         1.0f, -1.0f, -1.0f,
         1.0f, -1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,

        -1.0f, -1.0f,  1.0f,
        -1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f, -1.0f,  1.0f,
        -1.0f, -1.0f,  1.0f,

        -1.0f,  1.0f, -1.0f,
         1.0f,  1.0f, -1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
        -1.0f,  1.0f,  1.0f,
        -1.0f,  1.0f, -1.0f,

        -1.0f, -1.0f, -1.0f,
        -1.0f, -1.0f,  1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
        -1.0f, -1.0f,  1.0f,
         1.0f, -1.0f,  1.0f,
    };

    const float kQuadVertices[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
    };

    std::array<glm::mat4, 6> BuildCaptureViews()
    {
        return {
            glm::lookAt(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
            glm::lookAt(glm::vec3(0.0f), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
            glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f)),
            glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f)),
            glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
            glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
        };
    }
}

IBL::IBL(const char* hdrPath)
{
    CreateCaptureResources();

    try
    {
        LoadHdrEquirectangular(hdrPath);
        CreateEnvironmentCubemap();
        CreateIrradianceMap();
        CreatePrefilterMap();
        CreateBrdfLut();

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    catch (...)
    {
        DestroyResources();
        throw;
    }
}

IBL::~IBL()
{
    DestroyResources();
}

IBL::IBL(IBL&& other) noexcept
    : m_hdrTexture(other.m_hdrTexture),
      m_environmentCubemap(other.m_environmentCubemap),
      m_irradianceMap(other.m_irradianceMap),
      m_prefilterMap(other.m_prefilterMap),
      m_brdfLut(other.m_brdfLut),
      m_captureFbo(other.m_captureFbo),
      m_captureRbo(other.m_captureRbo),
      m_cubeVao(other.m_cubeVao),
      m_cubeVbo(other.m_cubeVbo),
      m_quadVao(other.m_quadVao),
      m_quadVbo(other.m_quadVbo),
      m_maxPrefilterMipLevel(other.m_maxPrefilterMipLevel),
      m_environmentIntensity(other.m_environmentIntensity)
{
    other.m_hdrTexture = 0;
    other.m_environmentCubemap = 0;
    other.m_irradianceMap = 0;
    other.m_prefilterMap = 0;
    other.m_brdfLut = 0;
    other.m_captureFbo = 0;
    other.m_captureRbo = 0;
    other.m_cubeVao = 0;
    other.m_cubeVbo = 0;
    other.m_quadVao = 0;
    other.m_quadVbo = 0;
}

IBL& IBL::operator=(IBL&& other) noexcept
{
    if (this != &other)
    {
        DestroyResources();
        m_hdrTexture = other.m_hdrTexture;
        m_environmentCubemap = other.m_environmentCubemap;
        m_irradianceMap = other.m_irradianceMap;
        m_prefilterMap = other.m_prefilterMap;
        m_brdfLut = other.m_brdfLut;
        m_captureFbo = other.m_captureFbo;
        m_captureRbo = other.m_captureRbo;
        m_cubeVao = other.m_cubeVao;
        m_cubeVbo = other.m_cubeVbo;
        m_quadVao = other.m_quadVao;
        m_quadVbo = other.m_quadVbo;
        m_maxPrefilterMipLevel = other.m_maxPrefilterMipLevel;
        m_environmentIntensity = other.m_environmentIntensity;

        other.m_hdrTexture = 0;
        other.m_environmentCubemap = 0;
        other.m_irradianceMap = 0;
        other.m_prefilterMap = 0;
        other.m_brdfLut = 0;
        other.m_captureFbo = 0;
        other.m_captureRbo = 0;
        other.m_cubeVao = 0;
        other.m_cubeVbo = 0;
        other.m_quadVao = 0;
        other.m_quadVbo = 0;
    }

    return *this;
}

void IBL::DestroyResources()
{
    if (m_hdrTexture != 0)
    {
        glDeleteTextures(1, &m_hdrTexture);
        m_hdrTexture = 0;
    }

    if (m_environmentCubemap != 0)
    {
        glDeleteTextures(1, &m_environmentCubemap);
        m_environmentCubemap = 0;
    }

    if (m_irradianceMap != 0)
    {
        glDeleteTextures(1, &m_irradianceMap);
        m_irradianceMap = 0;
    }

    if (m_prefilterMap != 0)
    {
        glDeleteTextures(1, &m_prefilterMap);
        m_prefilterMap = 0;
    }

    if (m_brdfLut != 0)
    {
        glDeleteTextures(1, &m_brdfLut);
        m_brdfLut = 0;
    }

    if (m_captureFbo != 0)
    {
        glDeleteFramebuffers(1, &m_captureFbo);
        m_captureFbo = 0;
    }

    if (m_captureRbo != 0)
    {
        glDeleteRenderbuffers(1, &m_captureRbo);
        m_captureRbo = 0;
    }

    if (m_cubeVao != 0)
    {
        glDeleteVertexArrays(1, &m_cubeVao);
        m_cubeVao = 0;
    }

    if (m_cubeVbo != 0)
    {
        glDeleteBuffers(1, &m_cubeVbo);
        m_cubeVbo = 0;
    }

    if (m_quadVao != 0)
    {
        glDeleteVertexArrays(1, &m_quadVao);
        m_quadVao = 0;
    }

    if (m_quadVbo != 0)
    {
        glDeleteBuffers(1, &m_quadVbo);
        m_quadVbo = 0;
    }
}

void IBL::CreateCaptureResources()
{
    glGenFramebuffers(1, &m_captureFbo);
    glGenRenderbuffers(1, &m_captureRbo);

    glGenVertexArrays(1, &m_cubeVao);
    glGenBuffers(1, &m_cubeVbo);
    glBindVertexArray(m_cubeVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_cubeVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kCubeVertices), kCubeVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), reinterpret_cast<void*>(0));
    glBindVertexArray(0);

    glGenVertexArrays(1, &m_quadVao);
    glGenBuffers(1, &m_quadVbo);
    glBindVertexArray(m_quadVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_quadVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuadVertices), kQuadVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float)));
    glBindVertexArray(0);
}

void IBL::LoadHdrEquirectangular(const char* hdrPath)
{
    stbi_set_flip_vertically_on_load(true);

    int width = 0;
    int height = 0;
    int channels = 0;
    float* imageData = stbi_loadf(hdrPath, &width, &height, &channels, 0);
    if (imageData == nullptr)
    {
        throw std::runtime_error(std::string("Failed to load HDR environment map: ") + hdrPath);
    }

    glGenTextures(1, &m_hdrTexture);
    glBindTexture(GL_TEXTURE_2D, m_hdrTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, width, height, 0, GL_RGB, GL_FLOAT, imageData);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    stbi_image_free(imageData);
}

void IBL::CaptureCubemapFaces(
    unsigned int targetCubemap,
    Shader& shader,
    unsigned int resolution,
    unsigned int mipLevel,
    bool generateMipmapsAfter)
{
    glBindFramebuffer(GL_FRAMEBUFFER, m_captureFbo);
    glBindRenderbuffer(GL_RENDERBUFFER, m_captureRbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, resolution, resolution);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_captureRbo);

    const glm::mat4 captureProjection = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);
    const auto captureViews = BuildCaptureViews();

    shader.Use();
    shader.SetMat4("uProjection", captureProjection);

    glViewport(0, 0, static_cast<GLsizei>(resolution), static_cast<GLsizei>(resolution));
    glBindVertexArray(m_cubeVao);

    for (unsigned int face = 0; face < 6; ++face)
    {
        shader.SetMat4("uView", captureViews[face]);
        glFramebufferTexture2D(
            GL_FRAMEBUFFER,
            GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
            targetCubemap,
            static_cast<GLint>(mipLevel));
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 36);
    }

    glBindVertexArray(0);

    if (generateMipmapsAfter)
    {
        glBindTexture(GL_TEXTURE_CUBE_MAP, targetCubemap);
        glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
    }
}

void IBL::CreateEnvironmentCubemap()
{
    glGenTextures(1, &m_environmentCubemap);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_environmentCubemap);
    for (unsigned int i = 0; i < 6; ++i)
    {
        glTexImage2D(
            GL_TEXTURE_CUBE_MAP_POSITIVE_X + i,
            0,
            GL_RGB16F,
            512,
            512,
            0,
            GL_RGB,
            GL_FLOAT,
            nullptr);
    }

    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    Shader equirectShader(
        EngineConstants::IblCubemapVertexShader,
        EngineConstants::IblEquirectToCubemapFragmentShader);
    equirectShader.Use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_hdrTexture);
    equirectShader.SetInt("uEquirectangularMap", 0);

    CaptureCubemapFaces(m_environmentCubemap, equirectShader, 512, 0, false);
}

void IBL::CreateIrradianceMap()
{
    glGenTextures(1, &m_irradianceMap);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_irradianceMap);
    for (unsigned int i = 0; i < 6; ++i)
    {
        glTexImage2D(
            GL_TEXTURE_CUBE_MAP_POSITIVE_X + i,
            0,
            GL_RGB16F,
            32,
            32,
            0,
            GL_RGB,
            GL_FLOAT,
            nullptr);
    }

    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    Shader irradianceShader(
        EngineConstants::IblCubemapVertexShader,
        EngineConstants::IblIrradianceFragmentShader);
    irradianceShader.Use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_environmentCubemap);
    irradianceShader.SetInt("uEnvironmentMap", 0);

    CaptureCubemapFaces(m_irradianceMap, irradianceShader, 32, 0, false);
}

void IBL::CreatePrefilterMap()
{
    const unsigned int prefilterResolution = 128;
    const unsigned int mipLevels = 5;
    m_maxPrefilterMipLevel = static_cast<float>(mipLevels - 1);

    glGenTextures(1, &m_prefilterMap);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_prefilterMap);
    for (unsigned int mip = 0; mip < mipLevels; ++mip)
    {
        const unsigned int mipWidth = prefilterResolution * static_cast<unsigned int>(std::pow(0.5, mip));
        for (unsigned int i = 0; i < 6; ++i)
        {
            glTexImage2D(
                GL_TEXTURE_CUBE_MAP_POSITIVE_X + i,
                static_cast<GLint>(mip),
                GL_RGB16F,
                mipWidth,
                mipWidth,
                0,
                GL_RGB,
                GL_FLOAT,
                nullptr);
        }
    }

    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    Shader prefilterShader(
        EngineConstants::IblCubemapVertexShader,
        EngineConstants::IblPrefilterFragmentShader);
    prefilterShader.Use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_environmentCubemap);
    prefilterShader.SetInt("uEnvironmentMap", 0);

    for (unsigned int mip = 0; mip < mipLevels; ++mip)
    {
        const unsigned int mipWidth = prefilterResolution * static_cast<unsigned int>(std::pow(0.5, mip));
        const float roughness = static_cast<float>(mip) / static_cast<float>(mipLevels - 1);
        prefilterShader.SetFloat("uRoughness", roughness);
        CaptureCubemapFaces(m_prefilterMap, prefilterShader, mipWidth, mip, false);
    }
}

void IBL::CreateBrdfLut()
{
    const unsigned int lutSize = 512;

    glGenTextures(1, &m_brdfLut);
    glBindTexture(GL_TEXTURE_2D, m_brdfLut);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, lutSize, lutSize, 0, GL_RG, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glBindFramebuffer(GL_FRAMEBUFFER, m_captureFbo);
    glBindRenderbuffer(GL_RENDERBUFFER, m_captureRbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, lutSize, lutSize);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_captureRbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_brdfLut, 0);

    glViewport(0, 0, static_cast<GLsizei>(lutSize), static_cast<GLsizei>(lutSize));
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    Shader brdfShader(EngineConstants::IblBrdfVertexShader, EngineConstants::IblBrdfFragmentShader);
    brdfShader.Use();
    glBindVertexArray(m_quadVao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

void IBL::BindTextures(Shader& shader) const
{
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_irradianceMap);
    shader.SetInt("uIrradianceMap", 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_prefilterMap);
    shader.SetInt("uPrefilterMap", 2);

    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, m_brdfLut);
    shader.SetInt("uBrdfLut", 3);

    shader.SetFloat("uMaxReflectionLod", m_maxPrefilterMipLevel);
    shader.SetFloat("uEnvironmentIntensity", m_environmentIntensity);
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
