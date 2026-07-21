#include "engine/raytracing/restir/RestirTypes.h"

#include <cstddef>
#include <iostream>

void RunRestirTypesTests(int& failures)
{
    if (sizeof(RestirGiReservoir) != 96)
    {
        std::cerr << "FAIL: RestirGiReservoir size is " << sizeof(RestirGiReservoir)
                  << ", expected 96\n";
        ++failures;
    }
    if (offsetof(RestirGiReservoir, radiance) != 16
        || offsetof(RestirGiReservoir, weightSum) != 28
        || offsetof(RestirGiReservoir, M) != 32
        || offsetof(RestirGiReservoir, instanceId) != 48
        || offsetof(RestirGiReservoir, initialPosition) != 64
        || offsetof(RestirGiReservoir, initialRadiance) != 80)
    {
        std::cerr << "FAIL: RestirGiReservoir field offsets do not match P5 layout\n";
        ++failures;
    }

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
    if (sizeof(RestirDiReservoirSet) != 128)
    {
        std::cerr << "FAIL: RestirDiReservoirSet size is " << sizeof(RestirDiReservoirSet)
                  << ", expected 128\n";
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
