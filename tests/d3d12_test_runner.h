#pragma once

#include <functional>
#include <string>
#include <vector>

namespace gpu_render_tests
{
    constexpr int kTierSmoke = 1;
    constexpr int kTierPbr = 2;
    constexpr int kTierEditor = 3;
    constexpr int kTierDxr = 4;
    constexpr int kTierPathTracing = 5;
    constexpr int kTierMax = kTierPathTracing;

    enum class TierSelectionMode
    {
        Through,
        Exact,
        Custom,
        All,
    };

    struct TestEntry
    {
        const char* name;
        int tier;
        const char* label;
        std::function<void()> run;
    };

    struct RunOptions
    {
        TierSelectionMode tierMode = TierSelectionMode::Through;
        int throughTier = kTierSmoke;
        int exactTier = kTierSmoke;
        std::vector<int> customTiers;
        std::string filter;
        bool listOnly = false;
        bool showHelp = false;
    };

    RunOptions ParseCommandLine(int argc, char** argv);
    void PrintHelp();
    void PrintTestList(const std::vector<TestEntry>& tests, const RunOptions& options);

    bool EntryMatchesTierSelection(int entryTier, const RunOptions& options);
    bool SelectionIncludesTierAtLeast(const RunOptions& options, int minTier);

    const std::vector<TestEntry>& GetTestRegistry();
    int Run(const RunOptions& options);
}
