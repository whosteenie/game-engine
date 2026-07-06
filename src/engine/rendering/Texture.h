#pragma once

#include "engine/rendering/TextureSamplerSettings.h"

#include <cstdint>
#include <memory>

enum class TextureColorSpace
{
    /// Linear data (normals, roughness, AO, masks) — uploaded as DXGI_FORMAT_R8G8B8A8_UNORM.
    Linear = 0,
    /// sRGB-encoded albedo — uploaded as DXGI_FORMAT_R8G8B8A8_UNORM_SRGB (GPU decodes on sample).
    SRGB = 1
};

class Texture
{
public:
    Texture(const char* path, TextureColorSpace colorSpace, bool flipVertically = true);
    Texture(
        const char* path,
        TextureColorSpace colorSpace,
        const TextureSamplerSettings& samplerSettings,
        bool flipVertically = true);
    static std::shared_ptr<Texture> CreateFromPixels(
        const unsigned char* pixels,
        int width,
        int height,
        int channelCount,
        TextureColorSpace colorSpace,
        const TextureSamplerSettings& samplerSettings = TextureSamplerSettings{},
        bool flipVertically = false);
    static void ReleaseUploadResources();
    ~Texture();

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    void Bind(unsigned int textureUnit) const;
    unsigned int GetId() const;
    bool IsValid() const;

    std::uintptr_t GetSrvCpuHandle() const { return m_srvCpuHandle; }
    std::uint32_t GetSrvDescriptorIndex() const { return m_srvDescriptorIndex; }

private:
    Texture() = default;

    void UploadPixels(
        const unsigned char* pixels,
        int width,
        int height,
        int channelCount,
        TextureColorSpace colorSpace);

    void* m_resource = nullptr;
    void* m_allocation = nullptr;
    void* m_uploadResource = nullptr;
    void* m_uploadAllocation = nullptr;
    std::uint32_t m_srvDescriptorIndex = UINT32_MAX;
    std::uintptr_t m_srvCpuHandle = 0;

    unsigned int m_id = 0;
    bool m_valid = false;
};
