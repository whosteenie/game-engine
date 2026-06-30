#pragma once

#include <string>

#ifdef _WIN32
struct HRESULT__;
using HRESULT = long;
#else
using HRESULT = long;
#endif

namespace HresultFormat
{
    std::string Format(HRESULT hr);
    std::string Describe(HRESULT hr);
    std::string FormatWithDescription(HRESULT hr);

    std::string DeviceRemovedMessage(const std::string& reason);
    std::string DeviceRemovedOpenProjectMessage(const std::string& reason);
    std::string FatalDeviceRemovedMessage(const std::string& reason);
}
