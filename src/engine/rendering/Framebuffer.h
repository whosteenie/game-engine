#pragma once

#include <cstdint>

enum class FramebufferColorMode
{
    Single,
    SplitDirectIndirect,
};

// Split-direct/indirect MRT layout (see devdoc/dxr-reflections.md binding table).
enum class GBufferSlot : int
{
    DirectLighting = 0,
    IndirectLighting = 1,
    ShadingNormal = 2,
    SunShadowFactor = 3,
    MotionVelocity = 4,
    MaterialAlbedoRough = 5,
    MaterialMetallic = 6,
};

inline int ToGBufferAttachmentIndex(const GBufferSlot slot)
{
    return static_cast<int>(slot);
}

class Framebuffer
{
public:
    Framebuffer() = default;
    ~Framebuffer();

    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;

    bool Resize(
        int width,
        int height,
        FramebufferColorMode colorMode = FramebufferColorMode::Single,
        int sampleCount = 1);
    void Bind() const;
    void Unbind() const;

    void ClearRenderTarget() const;
    void BindDrawTarget(bool clearAttachments = true, const float clearColor[4] = nullptr) const;
    void BindColorRenderTarget(bool clearAttachments = false, const float clearColor[4] = nullptr) const;
    bool BindGizmoDrawTarget() const;
    bool BindSplitLightingOverlayDrawTarget() const;
    bool CopyDepthFrom(const Framebuffer& source) const;
    void PrepareDepthForDepthTestPass() const;
    void PrepareResolvedDepthForDepthTestPass() const;
    void RestoreDepthShaderResource() const;
    void EnsureShaderResourceState() const;
    std::uintptr_t GetColorSrvCpuHandle(int attachmentIndex = 0) const;
    std::uintptr_t GetGBufferSrvCpuHandle(GBufferSlot slot) const;
    std::uintptr_t GetDepthSrvCpuHandle() const;
    void* GetColorResource(int attachmentIndex = 0) const;
    void* GetDepthResource() const;
    std::uintptr_t GetColorRtvCpuHandle(int attachmentIndex = 0) const;
    std::uintptr_t GetDepthDsvCpuHandle() const;
    std::uintptr_t GetResolvedDepthDsvCpuHandle() const;
    int GetSampleCount() const { return m_sampleCount; }
    bool UsesMsaa() const { return m_sampleCount > 1; }
    void ResolveMsaa() const;
    void BeginMsaaDepthResolvePass() const;
    void FinishMsaaDepthResolvePass() const;
    std::uintptr_t GetMsaaDepthSrvCpuHandle() const;
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
    bool HasVelocity() const;
    bool HasMaterialGbuffer() const;

    std::uintptr_t GetVelocityTexture() const;

    // Public for DXR passes: DispatchRays reads the resolved MRTs from non-pixel shaders, so
    // callers move attachments to a combined shader-read state first (tracked transitions).
    void TransitionColorAttachment(int attachmentIndex, std::uint32_t newState) const;
    void TransitionGBufferSlot(GBufferSlot slot, std::uint32_t newState) const;

private:
    void Destroy();
    void Create(int width, int height);

    void TransitionDepth(std::uint32_t newState) const;
    void TransitionMsaaColorAttachment(int attachmentIndex, std::uint32_t newState) const;
    void TransitionMsaaDepth(std::uint32_t newState) const;

    FramebufferColorMode m_colorMode = FramebufferColorMode::Single;
    int m_width = 0;
    int m_height = 0;
    int m_sampleCount = 1;

    static constexpr int MaxColorAttachments = 7;

    void* m_colorResources[MaxColorAttachments] = {};
    void* m_colorAllocations[MaxColorAttachments] = {};
    std::uint32_t m_colorSrvIndices[MaxColorAttachments] = {
        UINT32_MAX,
        UINT32_MAX,
        UINT32_MAX,
        UINT32_MAX,
        UINT32_MAX,
        UINT32_MAX,
        UINT32_MAX};
    void* m_depthResource = nullptr;
    void* m_depthAllocation = nullptr;
    std::uint32_t m_depthSrvIndex = UINT32_MAX;
    std::uint32_t m_msaaDepthSrvIndex = UINT32_MAX;
    std::uint32_t m_rtvBaseIndex = UINT32_MAX;
    std::uint32_t m_dsvIndex = UINT32_MAX;
    std::uint32_t m_resolvedRtvCount = 0;

    void* m_msaaColorResources[MaxColorAttachments] = {};
    void* m_msaaColorAllocations[MaxColorAttachments] = {};
    void* m_msaaDepthResource = nullptr;
    void* m_msaaDepthAllocation = nullptr;
    std::uint32_t m_msaaRtvBaseIndex = UINT32_MAX;
    std::uint32_t m_msaaDsvIndex = UINT32_MAX;
    mutable std::uint32_t m_msaaColorStates[MaxColorAttachments] = {};
    mutable std::uint32_t m_msaaDepthState = 0;
    int m_colorAttachmentCount = 1;
    mutable std::uint32_t m_colorStates[MaxColorAttachments] = {};
    mutable std::uint32_t m_depthState = 0;
};
