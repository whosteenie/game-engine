#pragma once

#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>

namespace test
{
    inline void SyncTestOutput()
    {
        std::cout << std::flush;
        std::cerr << std::flush;
    }

    inline int& FailureCount()
    {
        static int count = 0;
        return count;
    }

    inline int& TestRunCount()
    {
        static int count = 0;
        return count;
    }

    inline int& TestPassCount()
    {
        static int count = 0;
        return count;
    }

    inline void ExpectTrue(const bool condition, const char* message)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << message << "\n";
            SyncTestOutput();
            ++FailureCount();
        }
    }

    inline void ExpectNear(const float actual, const float expected, const float tolerance, const char* message)
    {
        if (std::abs(actual - expected) > tolerance)
        {
            std::cerr << "FAIL: " << message << " (actual=" << actual << " expected=" << expected << ")\n";
            SyncTestOutput();
            ++FailureCount();
        }
    }

    inline void ExpectContains(const std::string& haystack, const std::string& needle, const char* message)
    {
        if (haystack.find(needle) == std::string::npos)
        {
            std::cerr << "FAIL: " << message << " (haystack=\"" << haystack << "\")\n";
            SyncTestOutput();
            ++FailureCount();
        }
    }

    inline void ResetFailures()
    {
        FailureCount() = 0;
    }

    inline void ResetTestRun()
    {
        TestRunCount() = 0;
        TestPassCount() = 0;
    }

    inline void RunTest(const char* name, const std::function<void()>& body)
    {
        ++TestRunCount();
        const int failuresBefore = FailureCount();

        try
        {
            body();
        }
        catch (const std::exception& exception)
        {
            std::cerr << "[FAIL] " << name << " (exception: " << exception.what() << ")\n";
            SyncTestOutput();
            ++FailureCount();
            return;
        }
        catch (...)
        {
            std::cerr << "[FAIL] " << name << " (unknown exception)\n";
            SyncTestOutput();
            ++FailureCount();
            return;
        }

        if (FailureCount() > failuresBefore)
        {
            std::cerr << "[FAIL] " << name << "\n";
            SyncTestOutput();
            return;
        }

        ++TestPassCount();
        std::cout << "[PASS] " << name << "\n";
        SyncTestOutput();
    }

    inline void PrintSummary()
    {
        const int total = TestRunCount();
        const int passed = TestPassCount();
        const int failed = total - passed;

        std::cout << passed << "/" << total << " tests passed";
        if (failed > 0)
        {
            std::cout << " (" << failed << " failed)";
        }

        std::cout << ".\n";

        if (FailureCount() > 0)
        {
            std::cerr << FailureCount() << " assertion(s) failed.\n";
        }

        SyncTestOutput();
    }

    inline int ExitCode()
    {
        if (FailureCount() == 0 && TestPassCount() == TestRunCount())
        {
            return EXIT_SUCCESS;
        }

        return EXIT_FAILURE;
    }
}
