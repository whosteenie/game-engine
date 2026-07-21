#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

// S0-P5 capture contract.  This is deliberately a passive document format: it describes the
// already-selected renderer state and is never used to apply, clamp, or otherwise change it.
namespace CaptureManifest
{
    // v2 records the optical/mirror guide feature state and complete RR selection. These are
    // semantic inputs, so accepting a v1 replay would permit a non-equivalent renderer state.
    inline constexpr int SchemaVersion = 2;

    inline std::string NormalizeSemanticConfiguration(const nlohmann::json& semantic)
    {
        if (!semantic.is_object())
        {
            throw std::runtime_error("Capture manifest semantic configuration must be an object.");
        }
        // nlohmann::json's default object type is sorted by key. dump() is therefore a stable,
        // whitespace-free representation while arrays retain their meaningful order.
        return semantic.dump();
    }

    inline std::string ComputeSemanticHash(const nlohmann::json& semantic)
    {
        constexpr std::uint64_t offset = 14695981039346656037ull;
        constexpr std::uint64_t prime = 1099511628211ull;
        std::uint64_t hash = offset;
        for (const unsigned char value : NormalizeSemanticConfiguration(semantic))
        {
            hash ^= value;
            hash *= prime;
        }

        std::ostringstream stream;
        stream << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << hash;
        return stream.str();
    }

    inline void RequireExactObjectKeys(
        const nlohmann::json& object,
        const std::initializer_list<const char*> expected,
        const char* context)
    {
        if (!object.is_object())
        {
            throw std::runtime_error(std::string(context) + " must be an object.");
        }
        if (object.size() != expected.size())
        {
            throw std::runtime_error(std::string(context) + " contains missing or unknown fields.");
        }
        for (const char* name : expected)
        {
            if (!object.contains(name))
            {
                throw std::runtime_error(std::string(context) + " is missing required field '" + name + "'.");
            }
        }
    }

    inline void RequireNonEmptyString(const nlohmann::json& value, const char* context)
    {
        if (!value.is_string() || value.get_ref<const std::string&>().empty())
        {
            throw std::runtime_error(std::string(context) + " must be a non-empty string.");
        }
    }

    inline void ValidateSemanticConfiguration(const nlohmann::json& semantic)
    {
        RequireExactObjectKeys(
            semantic,
            {"revision", "scene", "camera", "viewport", "path_tracer", "restir", "reconstruction",
             "ser", "diagnostics", "window", "s0_capability"},
            "semantic");
        RequireNonEmptyString(semantic.at("revision"), "semantic.revision");

        RequireExactObjectKeys(semantic.at("scene"), {"project_path", "project_content_hash", "asset_manifest_content_hash"}, "semantic.scene");
        RequireNonEmptyString(semantic.at("scene").at("project_path"), "semantic.scene.project_path");
        RequireNonEmptyString(semantic.at("scene").at("project_content_hash"), "semantic.scene.project_content_hash");
        RequireNonEmptyString(semantic.at("scene").at("asset_manifest_content_hash"), "semantic.scene.asset_manifest_content_hash");

        RequireExactObjectKeys(semantic.at("camera"), {"owner", "pose_or_path"}, "semantic.camera");
        RequireNonEmptyString(semantic.at("camera").at("owner"), "semantic.camera.owner");
        if (!semantic.at("camera").at("pose_or_path").is_object())
        {
            throw std::runtime_error("semantic.camera.pose_or_path must be an object.");
        }

        RequireExactObjectKeys(semantic.at("viewport"), {"id", "output_extent", "render_extent"}, "semantic.viewport");
        RequireExactObjectKeys(
            semantic.at("path_tracer"),
            {"mode", "spp", "max_bounces", "seed", "russian_roulette", "firefly_clamp",
             "deterministic_optical_split", "optical_motion_replay", "mirror_chain_psr",
             "psr_max_bounces", "psr_subpixel_threshold"},
            "semantic.path_tracer");
        RequireExactObjectKeys(semantic.at("restir"), {"di_candidates", "di_temporal", "gi_initial", "gi_temporal", "gi_spatial", "diagnostic_mode"}, "semantic.restir");
        RequireExactObjectKeys(
            semantic.at("reconstruction"),
            {"feature", "anti_aliasing_mode", "quality", "rr_preset",
             "independent_optical_rr_layers", "rr_bundle_mode"},
            "semantic.reconstruction");
        RequireExactObjectKeys(semantic.at("ser"), {"requested_policy", "selected_permutation", "dispatched_permutation", "fallback_reason"}, "semantic.ser");
        RequireExactObjectKeys(semantic.at("diagnostics"), {"frame_trace", "gpu_events", "s0p2_token_trace", "s0p3_history_trace", "s0p4_scopes", "render_debug_mode"}, "semantic.diagnostics");
        RequireExactObjectKeys(semantic.at("window"), {"warmup_seconds", "warmup_frames", "sample_frames", "capture_frame_index"}, "semantic.window");
        if (!semantic.at("s0_capability").is_object())
        {
            throw std::runtime_error("semantic.s0_capability must be an object.");
        }
    }

    inline nlohmann::json Create(const nlohmann::json& semantic, const std::string& comparisonMode)
    {
        ValidateSemanticConfiguration(semantic);
        if (comparisonMode != "exact" && comparisonMode != "statistical")
        {
            throw std::runtime_error("Capture comparison mode must be 'exact' or 'statistical'.");
        }
        return nlohmann::json{
            {"record_type", "s0p5_capture_manifest"},
            {"schema_version", SchemaVersion},
            {"semantic", semantic},
            {"semantic_hash", ComputeSemanticHash(semantic)},
            {"comparison", {{"mode", comparisonMode}}},
        };
    }

    inline void Validate(const nlohmann::json& manifest)
    {
        RequireExactObjectKeys(
            manifest,
            {"record_type", "schema_version", "semantic", "semantic_hash", "comparison"},
            "capture manifest");
        if (manifest.at("record_type") != "s0p5_capture_manifest" || manifest.at("schema_version") != SchemaVersion)
        {
            throw std::runtime_error("Unsupported capture manifest record type or schema version.");
        }
        ValidateSemanticConfiguration(manifest.at("semantic"));
        RequireNonEmptyString(manifest.at("semantic_hash"), "capture manifest semantic_hash");
        if (manifest.at("semantic_hash") != ComputeSemanticHash(manifest.at("semantic")))
        {
            throw std::runtime_error("Capture manifest semantic_hash does not match normalized semantic configuration.");
        }
        RequireExactObjectKeys(manifest.at("comparison"), {"mode"}, "capture manifest comparison");
        const std::string mode = manifest.at("comparison").at("mode").get<std::string>();
        if (mode != "exact" && mode != "statistical")
        {
            throw std::runtime_error("Capture manifest comparison.mode must be 'exact' or 'statistical'.");
        }
    }

    inline nlohmann::json LoadAndValidate(const std::filesystem::path& path)
    {
        std::ifstream input(path);
        if (!input.is_open())
        {
            throw std::runtime_error("Could not open capture manifest: " + path.string());
        }

        nlohmann::json manifest;
        try
        {
            input >> manifest;
        }
        catch (const nlohmann::json::exception& exception)
        {
            throw std::runtime_error(
                "Could not parse capture manifest '" + path.string() + "': " + exception.what());
        }
        Validate(manifest);
        return manifest;
    }
}
