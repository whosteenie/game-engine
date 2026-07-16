#include "test_cli.h"
#include "test_expect.h"

#include <cstdlib>
#include <iostream>

namespace engine_tests_internal
{
    void RunShadowMapMathTests();
    void RunLightingProbeTests();
    void RunExceptionMessageTests();
}

void RunIrradianceShTests(int& failures);
void RunColorSpaceTests(int& failures);
void RunRotationUtilsTests(int& failures);
void RunDxrSettingsTests(int& failures);
void RunDirectionalShadowSettingsTests(int& failures);
void RunDxrAccelerationStructureTests(int& failures);
void RunPtTemporalHistoryTests(int& failures);
void RunPathRngTests(int& failures);
void RunRestirTypesTests(int& failures);
void RunRestirWrsTests(int& failures);
void RunRestirGiTests(int& failures);
void RunRestirTemporalChainTests(int& failures);
void RunRestirDiTests(int& failures);
void RunRestirSurfaceValidationTests(int& failures);
void RunDxrShaderInfrastructureTests(int& failures);
void RunMaterialTests();
void RunGuideEncodingTests();
void RunPayloadPackTests();
void RunRefractionTests();
void RunBrdfEnergyTests();
void RunMisNeeTests();
void RunEnvImportanceTests();
void RunNeeMisIntegrationTests();
void RunFrustumCullTests();
void RunCaptureManifestTests(int& failures);
void RunTemporalCameraPacketTests(int& failures);
void RunDlssFrameTokenTests(int& failures);
void RunHistoryCompatibilityTests(int& failures);
void RunViewportResizeStabilizerTests(int& failures);
void RunPtViewportOwnershipTests(int& failures);
void RunShaderCompileTests();

namespace
{
    void PrintHelp()
    {
        std::cout
            << "engine-tests — CPU unit / math suite\n\n"
            << "Usage:\n"
            << "  engine-tests [--filter=NAME] [--list]\n\n"
            << "Options:\n"
            << "  --filter=X   Run tests whose name contains X (* wildcards supported)\n"
            << "  --list       Print test names, then exit\n"
            << "  --help       Show this help\n";
    }

    void RunEngineTests(const test_cli::RunOptions& options)
    {
        auto maybeRun = [&](const char* name, const auto& body) {
            if (!test_cli::MatchesFilter(name, options.filter))
            {
                return;
            }

            if (options.listOnly)
            {
                std::cout << name << "  [cpu]\n";
                return;
            }

            test::RunTest(name, body);
        };

        maybeRun("shadow_map_math", engine_tests_internal::RunShadowMapMathTests);
        maybeRun("lighting_probe", engine_tests_internal::RunLightingProbeTests);
        maybeRun("exception_message", engine_tests_internal::RunExceptionMessageTests);
        maybeRun("irradiance_sh", [] { RunIrradianceShTests(test::FailureCount()); });
        maybeRun("color_space", [] { RunColorSpaceTests(test::FailureCount()); });
        maybeRun("rotation_utils", [] { RunRotationUtilsTests(test::FailureCount()); });
        maybeRun("dxr_settings", [] { RunDxrSettingsTests(test::FailureCount()); });
        maybeRun(
            "directional_shadow_settings",
            [] { RunDirectionalShadowSettingsTests(test::FailureCount()); });
        maybeRun(
            "dxr_acceleration_structure",
            [] { RunDxrAccelerationStructureTests(test::FailureCount()); });
        maybeRun("pt_temporal_history", [] { RunPtTemporalHistoryTests(test::FailureCount()); });
        maybeRun("path_rng", [] { RunPathRngTests(test::FailureCount()); });
        maybeRun("restir_types", [] { RunRestirTypesTests(test::FailureCount()); });
        maybeRun("restir_wrs", [] { RunRestirWrsTests(test::FailureCount()); });
        maybeRun("restir_gi", [] { RunRestirGiTests(test::FailureCount()); });
        maybeRun("restir_temporal_chain", [] { RunRestirTemporalChainTests(test::FailureCount()); });
        maybeRun("restir_di", [] { RunRestirDiTests(test::FailureCount()); });
        maybeRun("restir_surface_validation", [] { RunRestirSurfaceValidationTests(test::FailureCount()); });
        maybeRun(
            "dxr_shader_infrastructure",
            [] { RunDxrShaderInfrastructureTests(test::FailureCount()); });
        maybeRun("material", RunMaterialTests);
        maybeRun("guide_encoding", RunGuideEncodingTests);
        maybeRun("payload_pack", RunPayloadPackTests);
        maybeRun("refraction", RunRefractionTests);
        maybeRun("brdf_energy", RunBrdfEnergyTests);
        maybeRun("mis_nee", RunMisNeeTests);
        maybeRun("env_importance", RunEnvImportanceTests);
        maybeRun("nee_mis_integration", RunNeeMisIntegrationTests);
        maybeRun("frustum_cull", RunFrustumCullTests);
        maybeRun("capture_manifest", [] { RunCaptureManifestTests(test::FailureCount()); });
        maybeRun(
            "temporal_camera_packet",
            [] { RunTemporalCameraPacketTests(test::FailureCount()); });
        maybeRun("dlss_frame_token", [] { RunDlssFrameTokenTests(test::FailureCount()); });
        maybeRun(
            "history_compatibility",
            [] { RunHistoryCompatibilityTests(test::FailureCount()); });
        maybeRun(
            "viewport_resize_stabilizer",
            [] { RunViewportResizeStabilizerTests(test::FailureCount()); });
        maybeRun(
            "pt_viewport_ownership",
            [] { RunPtViewportOwnershipTests(test::FailureCount()); });
        maybeRun("shader_compile", RunShaderCompileTests);
    }
}

int main(const int argc, char** argv)
{
    const test_cli::RunOptions options = test_cli::ParseRunOptions(argc, argv, "engine-tests");

    if (options.showHelp)
    {
        PrintHelp();
        return EXIT_SUCCESS;
    }

    if (!options.listOnly)
    {
        test::ResetFailures();
        test::ResetTestRun();
    }

    RunEngineTests(options);

    if (options.listOnly)
    {
        return EXIT_SUCCESS;
    }

    test::PrintSummary();
    return test::ExitCode();
}
