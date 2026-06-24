#include "engine/rhi/d3d12/FixedDescriptorHeap.h"

#include <limits>

FixedDescriptorHeap::FixedDescriptorHeap(const std::uint32_t capacity)
    : m_capacity(capacity),
      m_allocated(capacity, false)
{
}

std::uint32_t FixedDescriptorHeap::AllocateOne()
{
    return AllocateBlock(1);
}

std::uint32_t FixedDescriptorHeap::AllocateBlock(const std::uint32_t count)
{
    if (count == 0 || m_capacity == 0 || count > m_capacity)
    {
        return kInvalid;
    }

    for (std::uint32_t start = 0; start + count <= m_capacity; ++start)
    {
        bool available = true;
        for (std::uint32_t offset = 0; offset < count; ++offset)
        {
            if (m_allocated[start + offset])
            {
                available = false;
                break;
            }
        }

        if (!available)
        {
            continue;
        }

        for (std::uint32_t offset = 0; offset < count; ++offset)
        {
            m_allocated[start + offset] = true;
        }

        m_usedCount += count;
        return start;
    }

    return kInvalid;
}

void FixedDescriptorHeap::FreeOne(const std::uint32_t index)
{
    FreeBlock(index, 1);
}

void FixedDescriptorHeap::FreeBlock(const std::uint32_t baseIndex, const std::uint32_t count)
{
    if (baseIndex == kInvalid || count == 0 || baseIndex >= m_capacity)
    {
        return;
    }

    for (std::uint32_t offset = 0; offset < count && baseIndex + offset < m_capacity; ++offset)
    {
        const std::uint32_t index = baseIndex + offset;
        if (m_allocated[index])
        {
            m_allocated[index] = false;
            --m_usedCount;
        }
    }
}

bool FixedDescriptorHeap::IsAllocated(const std::uint32_t index) const
{
    return IsValidIndex(index) && m_allocated[index];
}

bool FixedDescriptorHeap::IsValidIndex(const std::uint32_t index) const
{
    return index != kInvalid && index < m_capacity;
}
