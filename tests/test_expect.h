#pragma once

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace test
{
    inline int& FailureCount()
    {
        static int count = 0;
        return count;
    }

    inline void ExpectTrue(const bool condition, const char* message)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << message << "\n";
            ++FailureCount();
        }
    }

    inline void ExpectNear(const float actual, const float expected, const float tolerance, const char* message)
    {
        if (std::abs(actual - expected) > tolerance)
        {
            std::cerr << "FAIL: " << message << " (actual=" << actual << " expected=" << expected << ")\n";
            ++FailureCount();
        }
    }

    inline void ResetFailures()
    {
        FailureCount() = 0;
    }

    inline int ExitCode()
    {
        if (FailureCount() == 0)
        {
            return EXIT_SUCCESS;
        }

        std::cerr << FailureCount() << " test(s) failed.\n";
        return EXIT_FAILURE;
    }
}
