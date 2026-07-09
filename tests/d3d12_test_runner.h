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
    constexpr int kTierFull = 6;

    struct TestEntry
    {
        const char* name;
        int tier;
        const char* label;
        std::function<void()> run;
    };

    struct RunOptions
    {
        int maxTier = kTierSmoke;
        std::string filter;
        bool listOnly = false;
        bool showHelp = false;
    };

    RunOptions ParseCommandLine(int argc, char** argv);
    void PrintHelp();
    void PrintTestList(const std::vector<TestEntry>& tests, const RunOptions& options);

    const std::vector<TestEntry>& GetTestRegistry();
    int Run(const RunOptions& options);
}
