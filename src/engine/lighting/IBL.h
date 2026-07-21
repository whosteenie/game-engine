#pragma once

#include "engine/lighting/EnvironmentIblSettings.h"
#include "engine/lighting/IrradianceSh.h"
#include "engine/rhi/d3d12/GpuBuffer.h"

#include <cstdint>
#include <string>
#include <vector>

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
    // Per-frame render policy: when true, BindTextures tells the PBR pass to omit specular IBL
    // from the indirect output because a reflection composite (RT reflections / SSR) will add it
    // back at full precision (see uOmitSpecularIbl in pbr.ps.hlsl). Not persisted.
    void SetReflectionsReplaceSpecIbl(bool replace) { m_reflectionsReplaceSpecIbl = replace; }
    // Companion to the above for RT diffuse GI: omit the SH diffuse ambient from RT1 because the
    // GI inject will replace it (see uOmitDiffuseIbl in pbr.ps.hlsl). Not persisted.
    void SetGiReplacesDiffuseIbl(bool replace) { m_giReplacesDiffuseIbl = replace; }
    bool GetReflectionsReplaceSpecIbl() const { return m_reflectionsReplaceSpecIbl; }
    bool GetGiReplacesDiffuseIbl() const { return m_giReplacesDiffuseIbl; }
    const IrradianceSh9& GetIrradianceSh9() const { return m_irradianceSh; }

    bool IsReady() const;
    const std::string& GetLoadError() const;
    const std::string& GetHdrPath() const;
    float GetRotationYRadians() const;
    bool HasDetectedSunDirection() const { return m_hasDetectedSunDirection; }
    glm::vec3 GetDetectedSunDirection() const;
    std::uintptr_t GetEnvironmentCubemapSrvCpuHandle() const;
    std::uintptr_t GetPrefilterMapSrvCpuHandle() const;
    std::uintptr_t GetBrdfLutSrvCpuHandle() const;
    std::uintptr_t GetHdrEquirectSrvCpuHandle() const;

    // Path-tracer environment importance sampling (F2 / S5 step 13).
    bool HasEnvImportanceSampling() const { return m_envImportanceSampleCount > 0u; }
    std::uint32_t GetEnvImportanceCdfSrvIndex() const { return m_envImportanceCdfSrvIndex; }
    std::uint32_t GetEnvImportanceSampleCount() const { return m_envImportanceSampleCount; }
    int GetEnvImportanceCdfWidth() const { return m_envImportanceCdfWidth; }
    int GetEnvImportanceCdfHeight() const { return m_envImportanceCdfHeight; }
    float GetEnvImportanceWeightSum() const { return m_envImportanceWeightSum; }
    float GetEnvDirectLightingLuminanceClamp() const { return m_envDirectLightingLuminanceClamp; }

    void ReloadFromHdr(const char* hdrPath, float rotationYRadians = 0.0f);

    EnvironmentIblCubemapResolution GetCubemapResolutionMode() const;
    void SetCubemapResolutionMode(EnvironmentIblCubemapResolution mode);
    std::uint32_t GetCubemapFaceResolution() const;
    void GetHdrDimensions(int& width, int& height) const;

private:
    void EnsureCaptureDepthBuffer(std::uint32_t resolution);
    std::uint32_t ResolveCubemapFaceResolution() const;
    void CreateCaptureResources();
    void DestroyResources();
    void DestroyEnvironmentTextures();
    void LoadHdrEquirectangular(const char* hdrPath);
    void CreateEnvironmentCubemap();
    void CreatePrefilterMap();
    void CreateBrdfLut();
    void DestroyEnvImportanceCdf();
    void BuildAndUploadEnvImportanceCdf(const std::vector<float>& rgbaRadiance, int hdrWidth, int hdrHeight);

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
    IrradianceSh9 m_irradianceSh{};
    GpuTexture m_prefilterMapGpu{};
    GpuTexture m_brdfLutGpu{};

    GpuBuffer m_cubeVb{};
    GpuBuffer m_quadVb{};

    void* m_captureDepthResource = nullptr;
    void* m_captureDepthAllocation = nullptr;
    std::uint32_t m_captureDepthWidth = 0;
    std::uint32_t m_captureDepthDsvIndex = UINT32_MAX;
    std::uint32_t m_captureRtvIndex = UINT32_MAX;

    GpuTexture* m_activeCaptureTarget = nullptr;

    mutable bool m_gpuGenerated = false;
    std::string m_hdrPath;
    std::string m_loadError;
    float m_rotationYRadians = 0.0f;
    glm::vec3 m_detectedSunDirectionLocal{0.0f, 1.0f, 0.0f};
    bool m_hasDetectedSunDirection = false;

    float m_maxPrefilterMipLevel = 4.0f;
    float m_environmentIntensity = 0.4f;
    bool m_reflectionsReplaceSpecIbl = false;
    bool m_giReplacesDiffuseIbl = false;

    int m_hdrWidth = 0;
    int m_hdrHeight = 0;
    EnvironmentIblCubemapResolution m_cubemapResolutionMode = EnvironmentIblCubemapResolution::Auto;

    void* m_envImportanceCdfResource = nullptr;
    void* m_envImportanceCdfAllocation = nullptr;
    std::uint32_t m_envImportanceCdfSrvIndex = UINT32_MAX;
    std::uint32_t m_envImportanceSampleCount = 0;
    int m_envImportanceCdfWidth = 0;
    int m_envImportanceCdfHeight = 0;
    float m_envImportanceWeightSum = 0.0f;
    float m_envDirectLightingLuminanceClamp = 0.0f;
};
