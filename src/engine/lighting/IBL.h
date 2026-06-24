#pragma once

#include "engine/rhi/d3d12/GpuBuffer.h"

#include <cstdint>
#include <string>

class Shader;

class IBL
{
public:
    explicit IBL(const char* hdrPath);
    ~IBL();

    IBL(const IBL&) = delete;
    IBL& operator=(const IBL&) = delete;
    IBL(IBL&& other) noexcept;
    IBL& operator=(IBL&& other) noexcept;

    void BindTextures(Shader& shader) const;
    float GetMaxReflectionLod() const;
    float GetEnvironmentIntensity() const;
    void SetEnvironmentIntensity(float intensity);

private:
    void CreateCaptureResources();
    void DestroyResources();
    void LoadHdrEquirectangular(const char* hdrPath);
    void CreateEnvironmentCubemap();
    void CreateIrradianceMap();
    void CreatePrefilterMap();
    void CreateBrdfLut();

    void CaptureCubemapFaces(
        unsigned int targetCubemap,
        Shader& shader,
        unsigned int resolution,
        unsigned int mipLevel,
        bool generateMipmapsAfter);

    struct GpuTexture
    {
        void* resource = nullptr;
        void* allocation = nullptr;
        std::uint32_t srvDescriptorIndex = UINT32_MAX;
        std::uintptr_t srvCpuHandle = 0;
        std::uint32_t mipLevels = 1;
    };

    void DestroyGpuTexture(GpuTexture& texture);
    void GenerateGpuResources();
    GpuTexture CreateCubemapTextureResource(
        std::uint32_t resolution,
        std::uint32_t mipLevels,
        std::uint32_t initialState);
    GpuTexture CreateRenderTargetTexture2DResource(
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t format,
        std::uint32_t initialState);

    GpuTexture m_hdrGpu{};
    GpuTexture m_environmentCubemapGpu{};
    GpuTexture m_irradianceMapGpu{};
    GpuTexture m_prefilterMapGpu{};
    GpuTexture m_brdfLutGpu{};

    GpuBuffer m_cubeVb{};
    GpuBuffer m_quadVb{};

    void* m_captureDepthResource = nullptr;
    void* m_captureDepthAllocation = nullptr;
    std::uint32_t m_captureDepthDsvIndex = UINT32_MAX;
    std::uint32_t m_captureRtvIndex = UINT32_MAX;

    GpuTexture* m_activeCaptureTarget = nullptr;

    mutable bool m_gpuGenerated = false;
    std::string m_hdrPath;

    float m_maxPrefilterMipLevel = 4.0f;
    float m_environmentIntensity = 0.4f;
};
