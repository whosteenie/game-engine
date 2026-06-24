#pragma once

#include <stdexcept>
#include <string>

#include <wrl/client.h>

inline void ThrowIfFailed(HRESULT hr, const char* message)
{
    if (FAILED(hr))
    {
        throw std::runtime_error(std::string(message) + " (HRESULT=0x" + std::to_string(static_cast<unsigned long>(hr)) + ")");
    }
}

template<typename T>
void ThrowIfFailed(const Microsoft::WRL::ComPtr<T>& object, const char* message)
{
    if (object.Get() == nullptr)
    {
        throw std::runtime_error(message);
    }
}
