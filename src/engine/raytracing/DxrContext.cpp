#include "engine/raytracing/DxrContext.h"

#include "engine/rhi/GfxContext.h"

#include <d3d12.h>

DxrContext& DxrContext::Get()
{
    static DxrContext context;
    return context;
}

bool DxrContext::IsAvailable() const
{
    return GfxContext::Get().IsInitialized()
        && GfxContext::Get().IsRaytracingSupported()
        && GetDevice5() != nullptr;
}

ID3D12Device5* DxrContext::GetDevice5() const
{
    if (!GfxContext::Get().IsInitialized() || !GfxContext::Get().IsRaytracingSupported())
    {
        return nullptr;
    }

    if (!m_device5Queried)
    {
        m_device5Queried = true;
        auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
        if (device != nullptr)
        {
            device->QueryInterface(IID_PPV_ARGS(&m_device5));
        }
    }

    return m_device5;
}

ID3D12GraphicsCommandList4* DxrContext::QueryCommandList4(void* commandList) const
{
    if (commandList == nullptr)
    {
        return nullptr;
    }

    ID3D12GraphicsCommandList4* commandList4 = nullptr;
    static_cast<ID3D12GraphicsCommandList*>(commandList)->QueryInterface(IID_PPV_ARGS(&commandList4));
    return commandList4;
}
