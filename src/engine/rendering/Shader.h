#pragma once

#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(GAME_ENGINE_D3D12)
struct ID3D12PipelineState;
struct ID3D12RootSignature;
#endif

class Shader
{
public:
    Shader(const char* vertexPath, const char* fragmentPath);
    ~Shader();

    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    Shader(Shader&& other) noexcept;
    Shader& operator=(Shader&& other) noexcept;

    // viewportLdr selects the UNORM pipeline for the editor viewport framebuffer.
    void Use(bool mrtPass = false, bool viewportLdr = false) const;
#if defined(GAME_ENGINE_D3D12)
    // Binds PSO/root signature without clearing texture slots (post-process draws).
    void BindPipeline(bool mrtPass = false, bool viewportLdr = false) const;
#endif
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

    void BindTextureSlot(int unit, std::uintptr_t srvCpuHandle) const;

    unsigned int GetProgramId() const;
    bool IsLinked() const;
    bool HasUniform(const char* name) const;

#if defined(GAME_ENGINE_D3D12)
    ID3D12RootSignature* GetRootSignature() const;
    ID3D12PipelineState* GetPipelineState() const;
    static const Shader* GetActiveShader();
#endif

private:
#if defined(GAME_ENGINE_D3D12)
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

    std::vector<ConstantBuffer> m_constantBuffers;
    std::unordered_map<std::string, std::vector<UniformLocation>> m_uniformLocations;
    void* m_rootSignature = nullptr;
    void* m_pipelineState = nullptr;
    void* m_pipelineStateMrt = nullptr;
    void* m_pipelineStateLdr = nullptr;
    void* m_vertexShader = nullptr;
    void* m_pixelShader = nullptr;
    std::vector<std::uintptr_t> m_textureSlots;
    bool m_isLinked = false;
#else
    unsigned int m_programID = 0;
    bool m_isLinked = false;

    std::string ReadFile(const char* filepath) const;
    unsigned int Compile(unsigned int type, const char* source) const;
    unsigned int Link(unsigned int vertex, unsigned int fragment) const;
#endif
};
