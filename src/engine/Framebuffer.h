#pragma once

class Framebuffer
{
public:
    Framebuffer() = default;
    ~Framebuffer();

    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;

    void Resize(int width, int height);
    void Bind() const;
    void Unbind() const;

    unsigned int GetFramebuffer() const;
    unsigned int GetColorTexture() const;
    unsigned int GetDepthTexture() const;
    int GetWidth() const;
    int GetHeight() const;
    bool IsValid() const;

private:
    void Destroy();
    void Create(int width, int height);

    unsigned int m_fbo = 0;
    unsigned int m_colorTexture = 0;
    unsigned int m_depthTexture = 0;
    int m_width = 0;
    int m_height = 0;
};
