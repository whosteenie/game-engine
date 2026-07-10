#include "engine/rendering/PtTemporalHistory.h"

#include "engine/lighting/EnvironmentMap.h"
#include "engine/lighting/IBL.h"
#include "engine/rendering/DxrSettings.h"

#include <cstring>
#include <string>

namespace
{
    std::uint64_t HashCombine(const std::uint64_t seed, const std::uint64_t value)
    {
        return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
    }

    std::uint64_t HashFloatBits(const std::uint64_t seed, const float value)
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return HashCombine(seed, static_cast<std::uint64_t>(bits));
    }

    std::uint64_t HashString(const std::uint64_t seed, const std::string& value)
    {
        std::uint64_t fingerprint = HashCombine(seed, value.size());
        for (const unsigned char character : value)
        {
            fingerprint = HashCombine(fingerprint, static_cast<std::uint64_t>(character));
        }
        return fingerprint;
    }
}

std::uint64_t ComputePtEnvironmentFingerprint(const EnvironmentMap& environmentMap)
{
    std::uint64_t fingerprint = HashCombine(0, static_cast<std::uint32_t>(environmentMap.GetBackgroundMode()));
    fingerprint = HashString(fingerprint, environmentMap.GetHdrPath());
    fingerprint = HashFloatBits(fingerprint, environmentMap.GetRotationDegrees());
    fingerprint = HashFloatBits(fingerprint, environmentMap.GetExposure());
    fingerprint = HashCombine(
        fingerprint, static_cast<std::uint32_t>(environmentMap.GetIblCubemapResolution()));

    const glm::vec3 solidColor = environmentMap.GetSolidBackgroundColorSrgb();
    fingerprint = HashFloatBits(fingerprint, solidColor.x);
    fingerprint = HashFloatBits(fingerprint, solidColor.y);
    fingerprint = HashFloatBits(fingerprint, solidColor.z);

    const IBL& ibl = environmentMap.GetIBL();
    fingerprint = HashFloatBits(fingerprint, ibl.GetEnvironmentIntensity());
    fingerprint = HashCombine(fingerprint, ibl.IsReady() ? 1u : 0u);
    return fingerprint;
}

std::uint64_t ComputePtSettingsFingerprint(const DxrSettings& settings)
{
    std::uint64_t fingerprint = HashCombine(0, static_cast<std::uint32_t>(settings.GetPtConvergenceMode()));
    fingerprint = HashFloatBits(fingerprint, settings.GetMaxTraceDistance());
    fingerprint = HashCombine(fingerprint, static_cast<std::uint64_t>(settings.GetPtMaxBounces()));
    fingerprint = HashCombine(fingerprint, settings.IsPtRussianRouletteEnabled() ? 1u : 0u);
    fingerprint = HashCombine(fingerprint, settings.IsPtFireflyClampEnabled() ? 1u : 0u);
    fingerprint = HashFloatBits(fingerprint, settings.GetPtAmbientStrength());
    fingerprint = HashCombine(fingerprint, static_cast<std::uint64_t>(settings.GetPtAmbientAoRayCount()));
    fingerprint = HashCombine(fingerprint, static_cast<std::uint64_t>(settings.GetPtRrBundleMode()));
    return fingerprint;
}
