#include <Windows.h>

#include "d3d12_test_runner.h"

#include "d3d12_test_harness.h"
#include "engine/rhi/GfxContext.h"
#include "test_cli.h"
#include "test_expect.h"

#include <algorithm>
#include <cctype>
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
    namespace
    {
        std::string TrimCopy(std::string value)
        {
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
            {
                value.erase(value.begin());
            }

            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
            {
                value.pop_back();
            }

            return value;
        }

        bool TryParseTierValue(const std::string& text, int& outTier)
        {
            if (text.empty())
            {
                return false;
            }

            char* end = nullptr;
            const long value = std::strtol(text.c_str(), &end, 10);
            if (end == text.c_str() || (end != nullptr && *end != '\0'))
            {
                return false;
            }

            if (value < kTierSmoke || value > kTierMax)
            {
                return false;
            }

            outTier = static_cast<int>(value);
            return true;
        }

        bool AppendTierRange(const std::string& segment, std::vector<int>& outTiers)
        {
            const std::string trimmed = TrimCopy(segment);
            if (trimmed.empty())
            {
                return false;
            }

            const size_t dash = trimmed.find('-');
            if (dash == std::string::npos)
            {
                int tier = 0;
                if (!TryParseTierValue(trimmed, tier))
                {
                    return false;
                }

                outTiers.push_back(tier);
                return true;
            }

            int lowTier = 0;
            int highTier = 0;
            if (!TryParseTierValue(trimmed.substr(0, dash), lowTier)
                || !TryParseTierValue(trimmed.substr(dash + 1), highTier)
                || lowTier > highTier)
            {
                return false;
            }

            for (int tier = lowTier; tier <= highTier; ++tier)
            {
                outTiers.push_back(tier);
            }

            return true;
        }

        bool ParseCustomTiers(const std::string& expression, std::vector<int>& outTiers)
        {
            const std::string trimmed = TrimCopy(expression);
            if (trimmed.empty())
            {
                return false;
            }

            std::string lowered = trimmed;
            for (char& ch : lowered)
            {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }

            if (lowered == "all")
            {
                return true;
            }

            outTiers.clear();
            size_t start = 0;
            while (start <= trimmed.size())
            {
                const size_t comma = trimmed.find(',', start);
                const std::string segment =
                    trimmed.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                if (!AppendTierRange(segment, outTiers))
                {
                    return false;
                }

                if (comma == std::string::npos)
                {
                    break;
                }

                start = comma + 1;
            }

            std::sort(outTiers.begin(), outTiers.end());
            outTiers.erase(std::unique(outTiers.begin(), outTiers.end()), outTiers.end());
            return !outTiers.empty();
        }

        bool ParseCustomTiersOrAll(const std::string& expression, RunOptions& options)
        {
            const std::string trimmed = TrimCopy(expression);
            std::string lowered = trimmed;
            for (char& ch : lowered)
            {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }

            if (lowered == "all")
            {
                options.tierMode = TierSelectionMode::All;
                options.customTiers.clear();
                return true;
            }

            std::vector<int> tiers;
            if (!ParseCustomTiers(trimmed, tiers))
            {
                return false;
            }

            options.tierMode = TierSelectionMode::Custom;
            options.customTiers = std::move(tiers);
            return true;
        }
    }

    bool EntryMatchesTierSelection(const int entryTier, const RunOptions& options)
    {
        switch (options.tierMode)
        {
        case TierSelectionMode::Through:
            return entryTier >= kTierSmoke && entryTier <= options.throughTier;
        case TierSelectionMode::Exact:
            return entryTier == options.exactTier;
        case TierSelectionMode::Custom:
            return std::find(options.customTiers.begin(), options.customTiers.end(), entryTier)
                != options.customTiers.end();
        case TierSelectionMode::All:
            return true;
        default:
            return false;
        }
    }

    bool SelectionIncludesTierAtLeast(const RunOptions& options, const int minTier)
    {
        switch (options.tierMode)
        {
        case TierSelectionMode::Through:
            return options.throughTier >= minTier;
        case TierSelectionMode::Exact:
            return options.exactTier >= minTier;
        case TierSelectionMode::Custom:
            return std::any_of(
                options.customTiers.begin(),
                options.customTiers.end(),
                [minTier](const int tier) { return tier >= minTier; });
        case TierSelectionMode::All:
            return true;
        default:
            return false;
        }
    }

    RunOptions ParseCommandLine(const int argc, char** argv)
    {
        RunOptions options;
        bool tierSpecSeen = false;

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
                if (tierSpecSeen)
                {
                    std::cerr << "Only one tier selection flag may be used per run.\n";
                    options.showHelp = true;
                    continue;
                }

                tierSpecSeen = true;
                options.tierMode = TierSelectionMode::All;
                continue;
            }

            constexpr const char* kThroughPrefix = "--through=";
            if (arg.rfind(kThroughPrefix, 0) == 0)
            {
                if (tierSpecSeen)
                {
                    std::cerr << "Only one tier selection flag may be used per run.\n";
                    options.showHelp = true;
                    continue;
                }

                tierSpecSeen = true;
                options.tierMode = TierSelectionMode::Through;
                options.throughTier = std::atoi(arg.c_str() + std::strlen(kThroughPrefix));
                continue;
            }

            constexpr const char* kTierPrefix = "--tier=";
            if (arg.rfind(kTierPrefix, 0) == 0)
            {
                if (tierSpecSeen)
                {
                    std::cerr << "Only one tier selection flag may be used per run.\n";
                    options.showHelp = true;
                    continue;
                }

                tierSpecSeen = true;
                options.tierMode = TierSelectionMode::Exact;
                options.exactTier = std::atoi(arg.c_str() + std::strlen(kTierPrefix));
                continue;
            }

            constexpr const char* kTiersPrefix = "--tiers=";
            if (arg.rfind(kTiersPrefix, 0) == 0)
            {
                if (tierSpecSeen)
                {
                    std::cerr << "Only one tier selection flag may be used per run.\n";
                    options.showHelp = true;
                    continue;
                }

                tierSpecSeen = true;
                const std::string expression = arg.substr(std::strlen(kTiersPrefix));
                if (!ParseCustomTiersOrAll(expression, options))
                {
                    std::cerr << "Invalid --tiers expression: " << expression << "\n";
                    options.showHelp = true;
                }

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

        if (options.tierMode == TierSelectionMode::Through)
        {
            if (options.throughTier < kTierSmoke)
            {
                options.throughTier = kTierSmoke;
            }

            if (options.throughTier > kTierMax)
            {
                options.throughTier = kTierMax;
            }
        }
        else if (options.tierMode == TierSelectionMode::Exact)
        {
            if (options.exactTier < kTierSmoke)
            {
                options.exactTier = kTierSmoke;
            }

            if (options.exactTier > kTierMax)
            {
                options.exactTier = kTierMax;
            }
        }

        return options;
    }

    void PrintHelp()
    {
        std::cout
            << "d3d12-render-tests — GPU integration suite\n\n"
            << "Usage:\n"
            << "  d3d12-render-tests [tier selection] [--filter=PATTERN] [--list]\n\n"
            << "Tier selection (pick one; default: --through=1):\n"
            << "  --tier=N       Run only tier N (e.g. --tier=4 runs gpu-dxr tests only)\n"
            << "  --through=N    Run tiers 1 through N cumulatively (legacy default)\n"
            << "  --tiers=EXPR   Run a custom set: 1,2,4  |  1, 2, 4  |  2-4  |  1-3,5  |  all\n"
            << "  --all          Run every registered test\n\n"
            << "Other options:\n"
            << "  --filter=X     Name filter (* wildcard supported, e.g. TestPbr*)\n"
            << "  --list         Print tests that would run, then exit\n"
            << "  --help         Show this help\n\n"
            << "Tiers:\n"
            << "  1 gpu-smoke   clear, draw, upload, shader link\n"
            << "  2 gpu-pbr     PBR, shadows, IBL stability\n"
            << "  3 gpu-editor  editor path, present, resize, ImGui\n"
            << "  4 gpu-dxr     BLAS/TLAS + dispatch smoke\n"
            << "  5 gpu-dxr-pt  path tracing / glass / RR guides\n\n"
            << "Examples:\n"
            << "  d3d12-render-tests --tier=4\n"
            << "  d3d12-render-tests --through=2\n"
            << "  d3d12-render-tests --tiers=1,2,4\n"
            << "  d3d12-render-tests --tiers=\"1, 2, 4\"\n"
            << "  d3d12-render-tests --tiers=2-4\n"
            << "  d3d12-render-tests --all\n";
    }

    void PrintTestList(const std::vector<TestEntry>& tests, const RunOptions& options)
    {
        for (const TestEntry& entry : tests)
        {
            if (!EntryMatchesTierSelection(entry.tier, options))
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

        const bool needsPbrTier =
            SelectionIncludesTierAtLeast(options, kTierPbr)
            || std::any_of(
                tests.begin(),
                tests.end(),
                [&options](const TestEntry& entry) {
                    return EntryMatchesTierSelection(entry.tier, options)
                        && entry.tier >= kTierPbr
                        && test_cli::MatchesFilter(entry.name, options.filter);
                });

        if (needsPbrTier)
        {
            render_tests::SetFramebufferSizeForTier(kTierPbr);
            (void)session.GetEnvironmentIbl();
            GfxContext::Get().WaitForGpuIdle();
        }

        for (const TestEntry& entry : tests)
        {
            if (!EntryMatchesTierSelection(entry.tier, options))
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
