#include "app/editor/SettingRegistry.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace
{
    const std::vector<SettingRegistry::Descriptor> kDescriptors = {
        {"vsync", "Vertical sync", "vsync v sync", "Renderer Tuning", "Scene", SettingRegistry::ControlType::Checkbox, SettingRegistry::PersistenceScope::GlobalEditor, SettingRegistry::UndoPolicy::NotUndoable, true},
        {"environment_background", "Environment background", "skybox solid background", "Renderer Tuning", "Environment", SettingRegistry::ControlType::Dropdown, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, false},
        {"skybox_hdr_path", "Skybox HDR path", "hdr environment image panorama", "Renderer Tuning", "Environment", SettingRegistry::ControlType::FilePicker, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, false},
        {"skybox_rotation", "Skybox rotation", "sky hdr rotation", "Renderer Tuning", "Environment", SettingRegistry::ControlType::Slider, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, true},
        {"skybox_exposure", "Skybox exposure", "sky hdr exposure", "Renderer Tuning", "Environment", SettingRegistry::ControlType::Slider, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, true},
        {"ibl_cubemap_resolution", "IBL cubemap resolution", "reflection cubemap quality", "Renderer Tuning", "Environment", SettingRegistry::ControlType::Dropdown, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, false},
        {"environment_intensity", "Environment intensity", "ibl ambient environment", "Renderer Tuning", "Environment", SettingRegistry::ControlType::Slider, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, true},
        {"raytracing_enabled", "Enable ray tracing", "dxr ray tracing", "Renderer Tuning", "Ray tracing", SettingRegistry::ControlType::Checkbox, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, true},
        {"path_tracing", "Path tracing", "path traced rendering mode", "Renderer Tuning", "Ray tracing", SettingRegistry::ControlType::Dropdown, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, true},
        {"pt_ser_debug", "PT SER (debug)", "shader execution reordering path tracing diagnostic", "Renderer Tuning", "Ray tracing", SettingRegistry::ControlType::Dropdown, SettingRegistry::PersistenceScope::SessionOnly, SettingRegistry::UndoPolicy::NotUndoable, false},
        {"pt_convergence", "PT convergence", "path tracing realtime reference accumulate dlss rr", "Renderer Tuning", "Ray tracing", SettingRegistry::ControlType::Dropdown, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, true},
        {"pt_rr_inputs_debug", "RR inputs (debug)", "ray reconstruction diagnostic path tracing", "Renderer Tuning", "Ray tracing", SettingRegistry::ControlType::Dropdown, SettingRegistry::PersistenceScope::SessionOnly, SettingRegistry::UndoPolicy::NotUndoable, false},
        {"pt_max_bounces", "PT max bounces", "path tracing ray depth", "Renderer Tuning", "Ray tracing", SettingRegistry::ControlType::Slider, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, true},
        {"pt_russian_roulette", "PT Russian roulette", "path tracing termination", "Renderer Tuning", "Ray tracing", SettingRegistry::ControlType::Checkbox, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, false},
        {"pt_firefly_clamp", "PT firefly clamp", "path tracing bright sample clamp", "Renderer Tuning", "Ray tracing", SettingRegistry::ControlType::Checkbox, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, false},
        {"pt_ambient_strength", "PT ambient strength", "path tracing ambient", "Renderer Tuning", "Ray tracing", SettingRegistry::ControlType::Slider, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, false},
        {"pt_ambient_ao_rays", "PT ambient AO rays", "path tracing ambient occlusion", "Renderer Tuning", "Ray tracing", SettingRegistry::ControlType::Slider, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, false},
        {"pt_restir_di_candidates", "PT ReSTIR DI candidates", "path tracing direct illumination reservoir", "Renderer Tuning", "Ray tracing", SettingRegistry::ControlType::Slider, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, true},
        {"pt_restir_di_temporal", "PT ReSTIR DI temporal", "path tracing direct illumination temporal reuse", "Renderer Tuning", "Ray tracing", SettingRegistry::ControlType::Checkbox, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, true},
        {"pt_restir_gi_initial", "PT ReSTIR GI initial", "path tracing global illumination reservoir", "Renderer Tuning", "Ray tracing", SettingRegistry::ControlType::Checkbox, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, false},
        {"pt_restir_gi_temporal", "PT ReSTIR GI temporal", "path tracing global illumination temporal reuse", "Renderer Tuning", "Ray tracing", SettingRegistry::ControlType::Checkbox, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, false},
        {"sun_angular_radius", "Sun angular radius", "sun shadow softness penumbra", "Renderer Tuning", "Ray tracing", SettingRegistry::ControlType::Slider, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, false},
        {"rt_debug_trace", "Enable RT debug trace", "ray tracing diagnostic", "Renderer Tuning", "Ray tracing", SettingRegistry::ControlType::Checkbox, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, false},
        {"rt_reflections_enabled", "Enable RT reflections", "ray traced reflections", "Renderer Tuning", "Ray tracing", SettingRegistry::ControlType::Checkbox, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, true},
        {"rt_reflections_quality", "Reflections quality", "ray traced reflections quality", "Renderer Tuning", "Ray tracing", SettingRegistry::ControlType::Dropdown, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, false},
        {"rt_reflections_samples", "Reflection samples / pixel", "ray traced reflections spp", "Renderer Tuning", "Ray tracing", SettingRegistry::ControlType::Slider, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, false},
        {"rt_max_trace_distance", "Max trace distance", "ray tracing reflections distance", "Renderer Tuning", "Ray tracing", SettingRegistry::ControlType::Slider, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, false},
        {"rt_reflections_denoise", "Denoise enabled", "ray traced reflections denoiser", "Renderer Tuning", "Ray tracing", SettingRegistry::ControlType::Checkbox, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, false},
        {"rt_reflections_temporal_blend", "Temporal blend", "ray traced reflections denoiser history", "Renderer Tuning", "Ray tracing", SettingRegistry::ControlType::Slider, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, false},
        {"rt_reflections_atrous", "Denoiser smoothing (A-trous)", "ray traced reflections denoiser", "Renderer Tuning", "Ray tracing", SettingRegistry::ControlType::Slider, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, false},
        {"rt_reflections_anti_firefly", "Denoiser anti-firefly", "ray traced reflections denoiser clamp", "Renderer Tuning", "Ray tracing", SettingRegistry::ControlType::Checkbox, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, false},
        {"rt_reflections_ao_rays", "Reflection AO rays", "ray traced reflections occlusion", "Renderer Tuning", "Ray tracing", SettingRegistry::ControlType::Slider, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, false},
        {"rt_reflections_roughness_cutoff", "Reflection roughness cutoff", "ray traced reflections roughness", "Renderer Tuning", "Ray tracing", SettingRegistry::ControlType::Slider, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, false},
        {"rt_shadows_enabled", "Enable RT shadows", "ray traced shadows", "Renderer Tuning", "Ray tracing", SettingRegistry::ControlType::Checkbox, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, true},
        {"rt_shadow_denoise", "Shadow denoise (SIGMA)", "ray traced shadow denoiser", "Renderer Tuning", "Ray tracing", SettingRegistry::ControlType::Checkbox, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, false},
        {"rt_gi_enabled", "Enable RT GI", "ray traced global illumination", "Renderer Tuning", "Ray tracing", SettingRegistry::ControlType::Checkbox, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, true},
        {"rt_gi_strength", "GI strength", "ray traced global illumination", "Renderer Tuning", "Ray tracing", SettingRegistry::ControlType::Slider, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, false},
        {"rt_gi_denoise", "GI denoise (RELAX)", "ray traced global illumination denoiser", "Renderer Tuning", "Ray tracing", SettingRegistry::ControlType::Checkbox, SettingRegistry::PersistenceScope::SceneProject, SettingRegistry::UndoPolicy::Undoable, false},
    };

    std::string Lower(std::string_view value)
    {
        std::string result(value);
        std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return result;
    }
}

const std::vector<SettingRegistry::Descriptor>& SettingRegistry::GetAll() { return kDescriptors; }

const SettingRegistry::Descriptor* SettingRegistry::FindById(const std::string_view id)
{
    const auto it = std::find_if(kDescriptors.begin(), kDescriptors.end(),
        [id](const Descriptor& descriptor) { return descriptor.id == id; });
    return it != kDescriptors.end() ? &*it : nullptr;
}

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
