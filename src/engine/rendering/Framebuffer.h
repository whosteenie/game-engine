#pragma once

#include <cstdint>

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

    bool Resize(int width, int height, FramebufferColorMode colorMode = FramebufferColorMode::Single);
    void Bind() const;
    void Unbind() const;

    void ClearRenderTarget() const;
    void BindDrawTarget(bool clearAttachments = true) const;
    void EnsureShaderResourceState() const;
    std::uintptr_t GetColorSrvCpuHandle(int attachmentIndex = 0) const;
    std::uintptr_t GetDepthSrvCpuHandle() const;
    void* GetColorResource(int attachmentIndex = 0) const;
    void* GetDepthResource() const;
    std::uintptr_t GetColorRtvCpuHandle(int attachmentIndex = 0) const;
    std::uintptr_t GetDepthDsvCpuHandle() const;
    bool ReadbackColorPixel(int x, int y, float outRgba[4]) const;

    std::uintptr_t GetFramebuffer() const;
    std::uintptr_t GetColorTexture() const;
    std::uintptr_t GetIndirectColorTexture() const;
    std::uintptr_t GetNormalColorTexture() const;
    std::uintptr_t GetShadowFactorTexture() const;
    std::uintptr_t GetDepthTexture() const;
    int GetWidth() const;
    int GetHeight() const;
    bool IsValid() const;
    bool HasSplitLighting() const;
    bool HasGeometryNormals() const;
    bool HasShadowFactor() const;

private:
    void Destroy();
    void Create(int width, int height);

    void TransitionColorAttachment(int attachmentIndex, std::uint32_t newState) const;
    void TransitionDepth(std::uint32_t newState) const;

    FramebufferColorMode m_colorMode = FramebufferColorMode::Single;
    int m_width = 0;
    int m_height = 0;

    static constexpr int MaxColorAttachments = 4;

    void* m_colorResources[MaxColorAttachments] = {};
    void* m_colorAllocations[MaxColorAttachments] = {};
    std::uint32_t m_colorSrvIndices[MaxColorAttachments] = {
        UINT32_MAX,
        UINT32_MAX,
        UINT32_MAX,
        UINT32_MAX};
    void* m_depthResource = nullptr;
    void* m_depthAllocation = nullptr;
    std::uint32_t m_depthSrvIndex = UINT32_MAX;
    std::uint32_t m_rtvBaseIndex = UINT32_MAX;
    std::uint32_t m_dsvIndex = UINT32_MAX;
    int m_colorAttachmentCount = 1;
    mutable std::uint32_t m_colorStates[MaxColorAttachments] = {};
    mutable std::uint32_t m_depthState = 0;
};
