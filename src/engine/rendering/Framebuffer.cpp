#include <glad/glad.h>

#include "engine/rendering/Framebuffer.h"

Framebuffer::~Framebuffer()
{
    Destroy();
}

void Framebuffer::Destroy()
{
    if (m_depthTexture != 0)
    {
        glDeleteTextures(1, &m_depthTexture);
        m_depthTexture = 0;
    }

    if (m_indirectColorTexture != 0)
    {
        glDeleteTextures(1, &m_indirectColorTexture);
        m_indirectColorTexture = 0;
    }

    if (m_shadowFactorTexture != 0)
    {
        glDeleteTextures(1, &m_shadowFactorTexture);
        m_shadowFactorTexture = 0;
    }

    if (m_normalColorTexture != 0)
    {
        glDeleteTextures(1, &m_normalColorTexture);
        m_normalColorTexture = 0;
    }

    if (m_colorTexture != 0)
    {
        glDeleteTextures(1, &m_colorTexture);
        m_colorTexture = 0;
    }

    if (m_fbo != 0)
    {
        glDeleteFramebuffers(1, &m_fbo);
        m_fbo = 0;
    }

    m_width = 0;
    m_height = 0;
    m_colorMode = FramebufferColorMode::Single;
}

void Framebuffer::Create(const int width, const int height)
{
    glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);

    glGenTextures(1, &m_colorTexture);
    glBindTexture(GL_TEXTURE_2D, m_colorTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_colorTexture, 0);

    if (m_colorMode == FramebufferColorMode::SplitDirectIndirect)
    {
        glGenTextures(1, &m_indirectColorTexture);
        glBindTexture(GL_TEXTURE_2D, m_indirectColorTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, m_indirectColorTexture, 0);

        glGenTextures(1, &m_normalColorTexture);
        glBindTexture(GL_TEXTURE_2D, m_normalColorTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, m_normalColorTexture, 0);

        glGenTextures(1, &m_shadowFactorTexture);
        glBindTexture(GL_TEXTURE_2D, m_shadowFactorTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, width, height, 0, GL_RED, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3, GL_TEXTURE_2D, m_shadowFactorTexture, 0);

        const unsigned int attachments[] = {
            GL_COLOR_ATTACHMENT0,
            GL_COLOR_ATTACHMENT1,
            GL_COLOR_ATTACHMENT2,
            GL_COLOR_ATTACHMENT3};
        glDrawBuffers(4, attachments);
    }
    else
    {
        const unsigned int attachments[] = {GL_COLOR_ATTACHMENT0};
        glDrawBuffers(1, attachments);
    }

    glGenTextures(1, &m_depthTexture);
    glBindTexture(GL_TEXTURE_2D, m_depthTexture);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_DEPTH24_STENCIL8,
        width,
        height,
        0,
        GL_DEPTH_STENCIL,
        GL_UNSIGNED_INT_24_8,
        nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, m_depthTexture, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        Destroy();
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    m_width = width;
    m_height = height;
}

void Framebuffer::Resize(const int width, const int height, const FramebufferColorMode colorMode)
{
    if (width <= 0 || height <= 0)
    {
        return;
    }

    if (m_width == width && m_height == height && IsValid() && m_colorMode == colorMode)
    {
        return;
    }

    m_colorMode = colorMode;
    Destroy();
    m_colorMode = colorMode;
    Create(width, height);
}

void Framebuffer::Bind() const
{
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);

    if (!IsValid())
    {
        return;
    }

    if (m_colorMode == FramebufferColorMode::SplitDirectIndirect)
    {
        const unsigned int attachments[] = {
            GL_COLOR_ATTACHMENT0,
            GL_COLOR_ATTACHMENT1,
            GL_COLOR_ATTACHMENT2,
            GL_COLOR_ATTACHMENT3};
        glDrawBuffers(4, attachments);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glColorMaski(2, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glColorMaski(3, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }
    else
    {
        const unsigned int attachments[] = {GL_COLOR_ATTACHMENT0};
        glDrawBuffers(1, attachments);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }
}

void Framebuffer::Unbind() const
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

unsigned int Framebuffer::GetFramebuffer() const
{
    return m_fbo;
}

unsigned int Framebuffer::GetColorTexture() const
{
    return m_colorTexture;
}

unsigned int Framebuffer::GetIndirectColorTexture() const
{
    return m_indirectColorTexture;
}

unsigned int Framebuffer::GetNormalColorTexture() const
{
    return m_normalColorTexture;
}

unsigned int Framebuffer::GetShadowFactorTexture() const
{
    return m_shadowFactorTexture;
}

unsigned int Framebuffer::GetDepthTexture() const
{
    return m_depthTexture;
}

int Framebuffer::GetWidth() const
{
    return m_width;
}

int Framebuffer::GetHeight() const
{
    return m_height;
}

bool Framebuffer::IsValid() const
{
    return m_fbo != 0;
}

bool Framebuffer::HasSplitLighting() const
{
    return m_colorMode == FramebufferColorMode::SplitDirectIndirect && m_indirectColorTexture != 0;
}

bool Framebuffer::HasGeometryNormals() const
{
    return HasSplitLighting() && m_normalColorTexture != 0;
}

bool Framebuffer::HasShadowFactor() const
{
    return HasSplitLighting() && m_shadowFactorTexture != 0;
}
