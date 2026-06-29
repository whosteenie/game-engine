#pragma once

#include <cstdint>

struct ID3D12Device5;
struct ID3D12GraphicsCommandList4;

class DxrContext
{
public:
    static DxrContext& Get();

    bool IsAvailable() const;
    ID3D12Device5* GetDevice5() const;
    ID3D12GraphicsCommandList4* QueryCommandList4(void* commandList) const;

private:
    DxrContext() = default;

    mutable ID3D12Device5* m_device5 = nullptr;
    mutable bool m_device5Queried = false;
};
