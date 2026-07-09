#pragma once

#include <cstring>
#include <string>

namespace test_cli
{
    inline bool MatchesFilter(const std::string& name, const std::string& filter)
    {
        if (filter.empty())
        {
            return true;
        }

        if (filter.find('*') == std::string::npos)
        {
            return name == filter || name.find(filter) != std::string::npos;
        }

        if (filter.size() >= 2 && filter.front() == '*' && filter.back() == '*')
        {
            const std::string needle = filter.substr(1, filter.size() - 2);
            return name.find(needle) != std::string::npos;
        }

        if (!filter.empty() && filter.back() == '*')
        {
            const std::string prefix = filter.substr(0, filter.size() - 1);
            return name.compare(0, prefix.size(), prefix) == 0;
        }

        if (!filter.empty() && filter.front() == '*')
        {
            const std::string suffix = filter.substr(1);
            if (suffix.size() > name.size())
            {
                return false;
            }

            return name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

        return name == filter;
    }

    struct RunOptions
    {
        std::string filter;
        bool listOnly = false;
        bool showHelp = false;
    };

    inline RunOptions ParseRunOptions(int argc, char** argv, const char* programName, const char* extraHelp = "")
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

            constexpr const char* kFilterPrefix = "--filter=";
            if (arg.rfind(kFilterPrefix, 0) == 0)
            {
                options.filter = arg.substr(std::strlen(kFilterPrefix));
                continue;
            }

            (void)programName;
            (void)extraHelp;
        }

        return options;
    }
}
