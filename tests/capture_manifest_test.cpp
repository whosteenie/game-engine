#include "engine/platform/CaptureManifest.h"

#include <nlohmann/json.hpp>

namespace
{
    nlohmann::json MakeSemantic()
    {
        return {
            {"revision", "9b65f29127b818abaa142fbc851c157b409df02c"},
            {"scene", {{"project_path", "DEMO-SCENE.gameproject"}, {"project_content_hash", "a"}, {"asset_manifest_content_hash", "b"}}},
            {"camera", {{"owner", "SceneCamera::TryFromScene"}, {"pose_or_path", {{"position", {0, 1, 2}}}}}},
            {"viewport", {{"id", 1}, {"output_extent", {1920, 1080}}, {"render_extent", {1920, 1080}}}},
            {"path_tracer", {{"mode", "realTime"}, {"spp", 1}, {"max_bounces", 16}, {"seed", {{"algorithm", "pixel_frame"}}}, {"russian_roulette", true}, {"firefly_clamp", true}}},
            {"restir", {{"di_candidates", 1}, {"di_temporal", true}, {"gi_initial", true}, {"gi_temporal", true}, {"gi_spatial", false}, {"diagnostic_mode", "production"}}},
            {"reconstruction", {{"feature", "rr"}, {"quality", "dlaa"}, {"rr_bundle_mode", 0}}},
            {"ser", {{"requested_policy", "automatic"}, {"selected_permutation", "fallback"}, {"dispatched_permutation", "fallback"}, {"fallback_reason", "unsupported"}}},
            {"diagnostics", {{"frame_trace", true}, {"gpu_events", true}, {"s0p2_token_trace", true}, {"s0p3_history_trace", true}, {"s0p4_scopes", true}, {"render_debug_mode", 0}}},
            {"window", {{"warmup_seconds", 10}, {"warmup_frames", 120}, {"sample_frames", 300}, {"capture_frame_index", 421}}},
            {"s0_capability", {{"record_type", "dxr_runtime_snapshot"}}},
        };
    }
}

void RunCaptureManifestTests(int& failures)
{
    const nlohmann::json semantic = MakeSemantic();
    const nlohmann::json manifest = CaptureManifest::Create(semantic, "statistical");
    try { CaptureManifest::Validate(manifest); }
    catch (...) { ++failures; }

    nlohmann::json reordered = nlohmann::json::parse(semantic.dump());
    reordered = nlohmann::json::object();
    for (auto it = semantic.rbegin(); it != semantic.rend(); ++it) { reordered[it.key()] = it.value(); }
    if (CaptureManifest::ComputeSemanticHash(semantic) != CaptureManifest::ComputeSemanticHash(reordered)) { ++failures; }

    nlohmann::json unknown = manifest;
    unknown["semantic"]["unexpected"] = true;
    bool rejectedUnknown = false;
    try { CaptureManifest::Validate(unknown); }
    catch (...) { rejectedUnknown = true; }
    if (!rejectedUnknown) { ++failures; }

    nlohmann::json missing = manifest;
    missing["semantic"].erase("ser");
    bool rejectedMissing = false;
    try { CaptureManifest::Validate(missing); }
    catch (...) { rejectedMissing = true; }
    if (!rejectedMissing) { ++failures; }

    nlohmann::json tampered = manifest;
    tampered["semantic_hash"] = "fnv1a64:0000000000000000";
    bool rejectedTamper = false;
    try { CaptureManifest::Validate(tampered); }
    catch (...) { rejectedTamper = true; }
    if (!rejectedTamper) { ++failures; }
}
