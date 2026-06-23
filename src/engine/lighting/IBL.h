#pragma once

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

    unsigned int m_hdrTexture = 0;
    unsigned int m_environmentCubemap = 0;
    unsigned int m_irradianceMap = 0;
    unsigned int m_prefilterMap = 0;
    unsigned int m_brdfLut = 0;

    unsigned int m_captureFbo = 0;
    unsigned int m_captureRbo = 0;
    unsigned int m_cubeVao = 0;
    unsigned int m_cubeVbo = 0;
    unsigned int m_quadVao = 0;
    unsigned int m_quadVbo = 0;

    float m_maxPrefilterMipLevel = 4.0f;
    float m_environmentIntensity = 0.4f;
};
