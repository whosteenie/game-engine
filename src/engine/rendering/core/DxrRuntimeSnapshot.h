#pragma once

#include <array>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>

// S0-P1: versioned, machine-readable observability only.  This type deliberately carries the
// already-selected runtime state; it never participates in capability or PSO decisions.
struct DxrRuntimeSnapshot
{
    static constexpr std::uint32_t SchemaVersion = 1;

    struct PermutationResult
    {
        std::string compilerLibrary = "not_attempted";
        std::string rtpso = "not_attempted";
    };

    std::string adapterDescription = "missing";
    std::uint32_t adapterVendorId = 0;
    std::uint32_t adapterDeviceId = 0;
    std::string driverVersion = "missing";
    std::string osVersion = "missing";
    std::string agilityPackageVersion = "inbox_or_not_configured";
    std::string agilityLoaderVersion = "inbox_or_not_configured";
    int raytracingTier = 0;
    int highestShaderModel = 0;
    std::string options22Query = "not_attempted";
    bool options22ActuallyReorders = false;
    bool options22ByteOffsetViewsSupported = false;
    std::uint32_t options22Max1DDispatchSize = 0;
    std::uint32_t options22Max1DDispatchMeshSize = 0;
    std::array<PermutationResult, 4> permutations{};
    std::string requestedSerPolicy = "capability_selected";
    std::string selectedPermutation = "not_selected";
    std::string dispatchedPermutation = "not_dispatched";
    std::string fallbackReason = "not_evaluated";
};

inline constexpr std::size_t DxrRuntimeFallbackProduction = 0;
inline constexpr std::size_t DxrRuntimeFallbackDiagnostic = 1;
inline constexpr std::size_t DxrRuntimeSerProduction = 2;
inline constexpr std::size_t DxrRuntimeSerDiagnostic = 3;

inline std::string EscapeDxrRuntimeSnapshotJson(const std::string_view value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value)
    {
        switch (character)
        {
        case '\\': escaped += "\\\\"; break;
        case '\"': escaped += "\\\""; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (static_cast<unsigned char>(character) < 0x20)
            {
                escaped += "?";
            }
            else
            {
                escaped += character;
            }
            break;
        }
    }
    return escaped;
}

inline std::string SerializeDxrRuntimeSnapshotJson(const DxrRuntimeSnapshot& snapshot)
{
    const auto quoted = [](const std::string& value) {
        return std::string("\"") + EscapeDxrRuntimeSnapshotJson(value) + "\"";
    };
    const auto result = [&quoted](const DxrRuntimeSnapshot::PermutationResult& value) {
        return std::string("{\"compiler_library\":") + quoted(value.compilerLibrary)
            + ",\"rtpso\":" + quoted(value.rtpso) + "}";
    };

    std::ostringstream json;
    json << "{\"record_type\":\"dxr_runtime_snapshot\",\"schema_version\":"
         << DxrRuntimeSnapshot::SchemaVersion
         << ",\"adapter\":{\"description\":" << quoted(snapshot.adapterDescription)
         << ",\"vendor_id\":" << snapshot.adapterVendorId
         << ",\"device_id\":" << snapshot.adapterDeviceId
         << ",\"driver_version\":" << quoted(snapshot.driverVersion) << "}"
         << ",\"os\":{\"version\":" << quoted(snapshot.osVersion) << "}"
         << ",\"agility\":{\"package_version\":" << quoted(snapshot.agilityPackageVersion)
         << ",\"loader_version\":" << quoted(snapshot.agilityLoaderVersion) << "}"
         << ",\"dxr\":{\"tier\":" << snapshot.raytracingTier
         << ",\"highest_shader_model\":" << snapshot.highestShaderModel << "}"
         << ",\"options22\":{\"query\":" << quoted(snapshot.options22Query)
         << ",\"shader_execution_reordering_actually_reorders\":"
         << (snapshot.options22ActuallyReorders ? "true" : "false")
         << ",\"create_byte_offset_views_supported\":"
         << (snapshot.options22ByteOffsetViewsSupported ? "true" : "false")
         << ",\"max_1d_dispatch_size\":" << snapshot.options22Max1DDispatchSize
         << ",\"max_1d_dispatch_mesh_size\":" << snapshot.options22Max1DDispatchMeshSize << "}"
         << ",\"path_tracer\":{\"permutations\":{\"fallback_production\":"
         << result(snapshot.permutations[DxrRuntimeFallbackProduction])
         << ",\"fallback_diagnostic\":" << result(snapshot.permutations[DxrRuntimeFallbackDiagnostic])
         << ",\"ser_production\":" << result(snapshot.permutations[DxrRuntimeSerProduction])
         << ",\"ser_diagnostic\":" << result(snapshot.permutations[DxrRuntimeSerDiagnostic])
         << "},\"requested_ser_policy\":" << quoted(snapshot.requestedSerPolicy)
         << ",\"selected_permutation\":" << quoted(snapshot.selectedPermutation)
         << ",\"dispatched_permutation\":" << quoted(snapshot.dispatchedPermutation)
         << ",\"fallback_reason\":" << quoted(snapshot.fallbackReason) << "}}";
    return json.str();
}
