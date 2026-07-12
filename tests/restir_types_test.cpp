#include "engine/raytracing/RestirTypes.h"

#include <cstddef>
#include <iostream>

void RunRestirTypesTests(int& failures)
{
    if (sizeof(RestirInitialSample) != 32)
    {
        std::cerr << "FAIL: RestirInitialSample size is " << sizeof(RestirInitialSample)
                  << ", expected 32\n";
        ++failures;
    }

    if (sizeof(RestirReservoir) != 48)
    {
        std::cerr << "FAIL: RestirReservoir size is " << sizeof(RestirReservoir)
                  << ", expected 48\n";
        ++failures;
    }

    if (offsetof(RestirReservoir, sample) != 0
        || offsetof(RestirReservoir, wSum) != 32
        || offsetof(RestirReservoir, W) != 36
        || offsetof(RestirReservoir, M) != 40
        || offsetof(RestirReservoir, age) != 44)
    {
        std::cerr << "FAIL: RestirReservoir field offsets do not match G8 layout\n";
        ++failures;
    }
}
