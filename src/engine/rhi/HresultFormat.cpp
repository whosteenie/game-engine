#include "engine/rhi/HresultFormat.h"

#include <cstdio>
#include <iomanip>
#include <sstream>

namespace HresultFormat
{
    std::string Format(const HRESULT hr)
    {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<unsigned>(hr));
        return buffer;
    }

    std::string Describe(const HRESULT hr)
    {
        switch (hr)
        {
        case static_cast<HRESULT>(0x887A0001):
            return "DXGI_ERROR_INVALID_CALL";
        case static_cast<HRESULT>(0x887A0005):
            return "DXGI_ERROR_DEVICE_REMOVED";
        case static_cast<HRESULT>(0x887A0006):
            return "DXGI_ERROR_DEVICE_HUNG";
        case static_cast<HRESULT>(0x887A0007):
            return "DXGI_ERROR_DEVICE_RESET";
        case static_cast<HRESULT>(0x8007000E):
            return "E_OUTOFMEMORY";
        default:
            return {};
        }
    }

    std::string FormatWithDescription(const HRESULT hr)
    {
        const std::string formatted = Format(hr);
        const std::string described = Describe(hr);
        if (described.empty())
        {
            return formatted;
        }

        return formatted + " (" + described + ")";
    }

    std::string DeviceRemovedMessage(const std::string& reason)
    {
        return "D3D12 device was removed (" + reason + ")";
    }

    std::string DeviceRemovedOpenProjectMessage(const std::string& reason)
    {
        return "Cannot open project: " + DeviceRemovedMessage(reason) + ". Restart the editor.";
    }

    std::string FatalDeviceRemovedMessage(const std::string& reason)
    {
        return DeviceRemovedMessage(reason) + ". Restart the editor.";
    }
}
