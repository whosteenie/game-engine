#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <vector>

struct ID3D12PipelineState;
struct ID3D12RootSignature;

// Per-shader static-sampler overrides for the shared root signature (SSR-07 / REF-04).
// The default sampler table is built from the *material* texture filter settings with WRAP
// addressing on s4-s7 — correct for mesh materials, wrong for post-process passes that read
// depth/normal/velocity G-buffers or LUTs. Passes opt in to explicit sampler behavior here
// instead of relying on shader-filename sniffing.
struct ShaderSamplerOverrides
{
    // Bit N forces sampler register sN to point filtering (depth, normals, velocity,
    // variance — anything where bilinear blending across texels produces invalid values).
    std::uint32_t pointSampleRegisterMask = 0;
    // Forces CLAMP addressing on every register (post-process reads must not wrap across
    // screen or LUT edges).
    bool clampAllRegisters = false;
};

class Shader
{
public:
    Shader(const char* vertexPath, const char* fragmentPath);
    Shader(const char* vertexPath, const char* fragmentPath, const ShaderSamplerOverrides& samplerOverrides);
    ~Shader();

    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    Shader(Shader&& other) noexcept;
    Shader& operator=(Shader&& other) noexcept;

    // viewportLdr selects the UNORM pipeline for the editor viewport framebuffer.
    // depthReadOnly selects LDR + scene-depth testing (editor world gizmos).
    void Use(
        bool mrtPass = false,
        bool viewportLdr = false,
        bool doubleSided = false,
        bool depthReadOnly = false) const;
    // Binds PSO/root signature without clearing texture slots (post-process draws).
    void BindPipeline(
        bool mrtPass = false,
        bool viewportLdr = false,
        bool doubleSided = false,
        bool depthReadOnly = false) const;
    void FlushUniforms() const;
    void UseOnCommandList(void* commandList) const;
    void FlushUniformsOnCommandList(void* commandList) const;
    void SetFloat(const char* name, float value) const;
    void SetInt(const char* name, int value) const;
    void SetIntArray(const char* name, const int* values, int count) const;
    void SetFloatArray(const char* name, const float* values, int count) const;
    void SetMat4(const char* name, const glm::mat4& value) const;
    void SetMat4Array(const char* name, const glm::mat4* values, int count) const;
    void SetVec2(const char* name, const glm::vec2& value) const;
    void SetVec3(const char* name, const glm::vec3& value) const;
    void SetVec3Array(const char* name, const glm::vec3* values, int count) const;
    void SetVec4Array(const char* name, const glm::vec4* values, int count) const;

    void BindTextureSlot(int unit, std::uintptr_t srvCpuHandle) const;

    unsigned int GetProgramId() const;
    bool IsLinked() const;
    bool HasUniform(const char* name) const;

    ID3D12RootSignature* GetRootSignature() const;
    ID3D12PipelineState* GetPipelineState() const;
    static const Shader* GetActiveShader();

private:
    struct ConstantBuffer
    {
        void* resource = nullptr;
        void* allocation = nullptr;
        void* mapped = nullptr;
        std::uint32_t size = 0;
        std::uint32_t rootParameterIndex = 0;
        std::vector<std::uint8_t> staging;
    };

    struct UniformLocation
    {
        std::uint32_t bufferIndex = 0;
        std::uint32_t offset = 0;
        std::uint32_t size = 0;
    };

    void BuildFromHlsl(const std::string& vertexPath, const std::string& fragmentPath);
    void WriteUniform(const char* name, const void* data, std::uint32_t size) const;
    void WriteScalarArray(const char* name, const void* values, std::uint32_t elementSize, int count) const;

    std::vector<ConstantBuffer> m_constantBuffers;
    std::unordered_map<std::string, std::vector<UniformLocation>> m_uniformLocations;
    void* m_rootSignature = nullptr;
    void* m_pipelineState = nullptr;
    void* m_pipelineStateMrt = nullptr;
    void* m_pipelineStateLdr = nullptr;
    void* m_pipelineStateLdrDepthRead = nullptr;
    void* m_pipelineStateDoubleSided = nullptr;
    void* m_pipelineStateMrtDoubleSided = nullptr;
    void* m_pipelineStateLdrDoubleSided = nullptr;
    void* m_vertexShader = nullptr;
    void* m_pixelShader = nullptr;
    std::vector<std::uintptr_t> m_textureSlots;
    ShaderSamplerOverrides m_samplerOverrides{};
    bool m_isLinked = false;
};
