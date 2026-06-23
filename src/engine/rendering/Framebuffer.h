#pragma once

enum class FramebufferColorMode
{
    Single,
    SplitDirectIndirect,
};

class Framebuffer
{
public:
    Framebuffer() = default;
    ~Framebuffer();

    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;

    void Resize(int width, int height, FramebufferColorMode colorMode = FramebufferColorMode::Single);
    void Bind() const;
    void Unbind() const;

    unsigned int GetFramebuffer() const;
    unsigned int GetColorTexture() const;
    unsigned int GetIndirectColorTexture() const;
    unsigned int GetNormalColorTexture() const;
    unsigned int GetShadowFactorTexture() const;
    unsigned int GetDepthTexture() const;
    int GetWidth() const;
    int GetHeight() const;
    bool IsValid() const;
    bool HasSplitLighting() const;
    bool HasGeometryNormals() const;
    bool HasShadowFactor() const;

private:
    void Destroy();
    void Create(int width, int height);

    FramebufferColorMode m_colorMode = FramebufferColorMode::Single;
    unsigned int m_fbo = 0;
    unsigned int m_colorTexture = 0;
    unsigned int m_indirectColorTexture = 0;
    unsigned int m_normalColorTexture = 0;
    unsigned int m_shadowFactorTexture = 0;
    unsigned int m_depthTexture = 0;
    int m_width = 0;
    int m_height = 0;
};
