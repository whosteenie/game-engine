#pragma once

#include <cstddef>
#include <string>

namespace EnvironmentPresets
{
    struct Entry
    {
        const char* label;
        const char* path;
    };

    inline constexpr Entry kEntries[] = {
        {"Custom", ""},
        // 2K — general presets
        {"Outdoor Clear (Qwantani)", "assets/environment/qwantani_puresky_2k.hdr"},
        {"Partly Cloudy (Kloofendal)", "assets/environment/kloofendal_48d_partly_cloudy_2k.hdr"},
        {"Overcast", "assets/environment/overcast_soil_puresky_2k.hdr"},
        {"Mountain (Drakensberg)", "assets/environment/drakensberg_solitary_mountain_puresky_2k.hdr"},
        {"Night Kloppenheim", "assets/environment/kloppenheim_02_2k.hdr"},
        // 2K — scenes with visible environment (studio, water, urban, etc.)
        {"Studio Indoor", "assets/environment/studio_small_09_2k.hdr"},
        {"Sunset (Venice)", "assets/environment/venice_sunset_2k.hdr"},
        {"Sunrise (Spruit)", "assets/environment/spruit_sunrise_2k.hdr"},
        {"Garden", "assets/environment/symmetrical_garden_2k.hdr"},
        {"Tropical (Blue Lagoon)", "assets/environment/blue_lagoon_2k.hdr"},
        {"Night Golf Course", "assets/environment/moonless_golf_2k.hdr"},
        {"Night Dikhololo", "assets/environment/dikhololo_night_2k.hdr"},
        {"Night Rooftop", "assets/environment/rooftop_night_2k.hdr"},
        // 4K — full-sky / puresky (no built environment in frame)
        {"4K Outdoor Clear (Qwantani)", "assets/environment/qwantani_puresky_4k.hdr"},
        {"4K Overcast", "assets/environment/overcast_soil_puresky_4k.hdr"},
        {"4K Partly Cloudy (Kloofendal)", "assets/environment/kloofendal_48d_partly_cloudy_4k.hdr"},
        {"4K Clear Day (Syferfontein)", "assets/environment/syferfontein_18d_clear_4k.hdr"},
        {"4K Mountain Alpine", "assets/environment/drakensberg_solitary_mountain_puresky_4k.hdr"},
        {"4K Mud Road Clear", "assets/environment/mud_road_puresky_4k.hdr"},
        {"4K Starry Night (Kloppenheim)", "assets/environment/kloppenheim_02_4k.hdr"},
        {"4K Clear Night (Rogland)", "assets/environment/rogland_clear_night_4k.hdr"},
    };

    inline constexpr std::size_t kCount = sizeof(kEntries) / sizeof(kEntries[0]);

    inline std::string EnvironmentStem(const std::string& path)
    {
        std::string filename = path;
        const std::size_t slash = filename.find_last_of("/\\");
        if (slash != std::string::npos)
        {
            filename = filename.substr(slash + 1);
        }

        constexpr const char* kResolutionSuffixes[] = {"_4k.hdr", "_2k.hdr", "_1k.hdr"};
        for (const char* suffix : kResolutionSuffixes)
        {
            const std::size_t suffixLength = std::char_traits<char>::length(suffix);
            if (filename.size() >= suffixLength
                && filename.compare(filename.size() - suffixLength, suffixLength, suffix) == 0)
            {
                return filename.substr(0, filename.size() - suffixLength);
            }
        }

        if (filename.size() > 4 && filename.compare(filename.size() - 4, 4, ".hdr") == 0)
        {
            return filename.substr(0, filename.size() - 4);
        }

        return filename;
    }

    inline int FindPresetIndex(const std::string& path)
    {
        if (path.empty())
        {
            return 0;
        }

        for (std::size_t index = 1; index < kCount; ++index)
        {
            if (path == kEntries[index].path)
            {
                return static_cast<int>(index);
            }
        }

        const std::string pathStem = EnvironmentStem(path);
        for (std::size_t index = 1; index < kCount; ++index)
        {
            if (pathStem == EnvironmentStem(kEntries[index].path))
            {
                return static_cast<int>(index);
            }
        }

        return 0;
    }
}
