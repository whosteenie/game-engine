#include "app/editor/SettingRegistry.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace
{
    const std::vector<SettingRegistry::Descriptor> kDescriptors = {
        {"vsync", "Vertical sync", "vsync v sync", "Renderer Tuning", "Scene", SettingRegistry::ControlType::Checkbox, SettingRegistry::PersistenceScope::GlobalEditor, SettingRegistry::UndoPolicy::NotUndoable, true},
        {"skybox_rotation", "Skybox rotation", "sky hdr rotation", "Renderer Tuning", "Environment", SettingRegistry::ControlType::Slider, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, true},
        {"skybox_exposure", "Skybox exposure", "sky hdr exposure", "Renderer Tuning", "Environment", SettingRegistry::ControlType::Slider, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, true},
        {"environment_intensity", "Environment intensity", "ibl ambient environment", "Renderer Tuning", "Environment", SettingRegistry::ControlType::Slider, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, true},
        {"path_tracing", "Path tracing", "path traced rendering mode", "Renderer Tuning", "Ray tracing", SettingRegistry::ControlType::Dropdown, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, true},
    };

    std::string Lower(std::string_view value)
    {
        std::string result(value);
        std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return result;
    }
}

const std::vector<SettingRegistry::Descriptor>& SettingRegistry::GetAll() { return kDescriptors; }

std::vector<const SettingRegistry::Descriptor*> SettingRegistry::FindSearchMatches(const std::string_view query)
{
    const std::string needle = Lower(query);
    std::vector<const Descriptor*> matches;
    if (needle.empty()) return matches;
    for (const Descriptor& descriptor : kDescriptors)
    {
        if (!descriptor.searchIndexed) continue;
        const std::string haystack = Lower(std::string(descriptor.label) + " " + std::string(descriptor.keywords));
        if (haystack.find(needle) != std::string::npos) matches.push_back(&descriptor);
    }
    return matches;
}
