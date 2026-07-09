#include "d3d12_test_runner.h"

#include "d3d12_test_harness.h"
#include "engine/rhi/GfxContext.h"
#include "test_cli.h"
#include "test_expect.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace render_tests
{
    void RegisterGpuRenderTests(std::vector<gpu_render_tests::TestEntry>& outTests);
    void SetFramebufferSizeForTier(int tier);
}

namespace gpu_render_tests
{
    RunOptions ParseCommandLine(const int argc, char** argv)
    {
        RunOptions options;

        for (int index = 1; index < argc; ++index)
        {
            const std::string arg = argv[index];
            if (arg == "--help" || arg == "-h")
            {
                options.showHelp = true;
                continue;
            }

            if (arg == "--list")
            {
                options.listOnly = true;
                continue;
            }

            if (arg == "--all")
            {
                options.maxTier = kTierFull;
                continue;
            }

            constexpr const char* kTierPrefix = "--tier=";
            if (arg.rfind(kTierPrefix, 0) == 0)
            {
                options.maxTier = std::atoi(arg.c_str() + std::strlen(kTierPrefix));
                continue;
            }

            constexpr const char* kFilterPrefix = "--filter=";
            if (arg.rfind(kFilterPrefix, 0) == 0)
            {
                options.filter = arg.substr(std::strlen(kFilterPrefix));
                continue;
            }

            std::cerr << "Unknown argument: " << arg << "\n";
            options.showHelp = true;
        }

        if (options.maxTier < kTierSmoke)
        {
            options.maxTier = kTierSmoke;
        }

        if (options.maxTier > kTierFull)
        {
            options.maxTier = kTierFull;
        }

        return options;
    }

    void PrintHelp()
    {
        std::cout
            << "d3d12-render-tests — GPU integration suite\n\n"
            << "Usage:\n"
            << "  d3d12-render-tests [--tier=N] [--filter=PATTERN] [--list] [--all]\n\n"
            << "Options:\n"
            << "  --tier=N     Run tiers 1..N (default: 1 = gpu-smoke only)\n"
            << "  --all        Run every tier (same as --tier=6)\n"
            << "  --filter=X   Name filter (* wildcard supported, e.g. TestPbr*)\n"
            << "  --list       Print tests that would run, then exit\n"
            << "  --help       Show this help\n\n"
            << "Tiers:\n"
            << "  1 gpu-smoke   clear, draw, upload, shader link\n"
            << "  2 gpu-pbr     PBR, shadows, IBL stability\n"
            << "  3 gpu-editor  editor path, present, resize, ImGui\n"
            << "  4 gpu-dxr     BLAS/TLAS + dispatch smoke\n"
            << "  5 gpu-dxr-pt  path tracing / glass (future)\n"
            << "  6 gpu-full    all of the above\n";
    }

    void PrintTestList(const std::vector<TestEntry>& tests, const RunOptions& options)
    {
        for (const TestEntry& entry : tests)
        {
            if (entry.tier > options.maxTier)
            {
                continue;
            }

            if (!test_cli::MatchesFilter(entry.name, options.filter))
            {
                continue;
            }

            std::cout << "T" << entry.tier << "  " << entry.name << "  [" << entry.label << "]\n";
        }
    }

    const std::vector<TestEntry>& GetTestRegistry()
    {
        static std::vector<TestEntry> tests;
        if (tests.empty())
        {
            render_tests::RegisterGpuRenderTests(tests);
        }

        return tests;
    }

    int Run(const RunOptions& options)
    {
        if (options.showHelp)
        {
            PrintHelp();
            return EXIT_SUCCESS;
        }

        const std::vector<TestEntry>& tests = GetTestRegistry();
        if (options.listOnly)
        {
            PrintTestList(tests, options);
            return EXIT_SUCCESS;
        }

        D3d12TestSession& session = GetD3d12TestSession();
        if (!session.EnsureInitialized())
        {
            std::cerr << "FAIL: D3D12 test session failed to initialize\n";
            return EXIT_FAILURE;
        }

        test::ResetFailures();
        test::ResetTestRun();

        if (options.maxTier >= kTierPbr)
        {
            render_tests::SetFramebufferSizeForTier(kTierPbr);
            (void)session.GetEnvironmentIbl();
            GfxContext::Get().WaitForGpuIdle();
        }

        for (const TestEntry& entry : tests)
        {
            if (entry.tier > options.maxTier)
            {
                continue;
            }

            if (!test_cli::MatchesFilter(entry.name, options.filter))
            {
                continue;
            }

            render_tests::SetFramebufferSizeForTier(entry.tier);
            test::RunTest(entry.name, entry.run);
        }

        test::PrintSummary();

        FinalizeD3d12TestSession();
        session.Shutdown();

        return test::ExitCode();
    }
}

int main(const int argc, char** argv)
{
    const gpu_render_tests::RunOptions options = gpu_render_tests::ParseCommandLine(argc, argv);
    return gpu_render_tests::Run(options);
}
