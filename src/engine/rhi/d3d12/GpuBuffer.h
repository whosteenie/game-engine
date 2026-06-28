#pragma once

#include <cstdint>

namespace D3D12MA
{
class Allocation;
}

class GpuBuffer
{
public:
    enum class Type
    {
        Vertex,
        Index,
    };

    GpuBuffer() = default;
    ~GpuBuffer();

    GpuBuffer(const GpuBuffer&) = delete;
    GpuBuffer& operator=(const GpuBuffer&) = delete;
    GpuBuffer(GpuBuffer&& other) noexcept;
    GpuBuffer& operator=(GpuBuffer&& other) noexcept;

    void Create(Type type, const void* data, std::uint32_t byteSize);
    void CreateUpload(Type type, const void* data, std::uint32_t byteSize);
    void Destroy();

    void BindVertex(std::uint32_t slot, std::uint32_t stride) const;
    void BindIndex() const;
    void BindVertexToCommandList(void* commandList, std::uint32_t slot, std::uint32_t stride) const;

    bool IsValid() const { return m_resource != nullptr; }

private:
    void* m_resource = nullptr;
    D3D12MA::Allocation* m_allocation = nullptr;
    void* m_uploadResource = nullptr;
    D3D12MA::Allocation* m_uploadAllocation = nullptr;
    Type m_type = Type::Vertex;
    std::uint32_t m_byteSize = 0;
};
