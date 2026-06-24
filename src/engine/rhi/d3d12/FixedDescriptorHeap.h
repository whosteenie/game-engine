#pragma once

#include <cstdint>
#include <vector>

// CPU-side bookkeeping for a fixed-size D3D12 descriptor heap.
// Returns UINT32_MAX when exhausted so callers never write past heap bounds.
class FixedDescriptorHeap
{
public:
    static constexpr std::uint32_t kInvalid = UINT32_MAX;

    explicit FixedDescriptorHeap(std::uint32_t capacity = 0);

    std::uint32_t Capacity() const { return m_capacity; }
    std::uint32_t UsedCount() const { return m_usedCount; }

    std::uint32_t AllocateOne();
    std::uint32_t AllocateBlock(std::uint32_t count);
    void FreeOne(std::uint32_t index);
    void FreeBlock(std::uint32_t baseIndex, std::uint32_t count);
    bool IsAllocated(std::uint32_t index) const;
    bool IsValidIndex(std::uint32_t index) const;

private:
    std::uint32_t m_capacity = 0;
    std::uint32_t m_usedCount = 0;
    std::vector<bool> m_allocated;
};
