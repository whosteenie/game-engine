#include "engine/rendering/post/ReconstructionExposurePolicy.h"

#include <cmath>
#include <iostream>

namespace
{
    void ExpectNear(
        int& failures,
        const float actual,
        const float expected,
        const char* message,
        const float epsilon = 0.0001f)
    {
        if (std::fabs(actual - expected) <= epsilon)
        {
            return;
        }
        std::cerr << "FAIL: " << message << " (actual=" << actual
                  << ", expected=" << expected << ")\n";
        ++failures;
    }
}

void RunReconstructionExposureTests(int& failures)
{
    constexpr float kHdrPatch = 0.125f;
    const float evMinusTwo = ApplyDisplayExposure(kHdrPatch, -2.0f);
    const float evZero = ApplyDisplayExposure(kHdrPatch, 0.0f);
    const float evPlusTwo = ApplyDisplayExposure(kHdrPatch, 2.0f);

    ExpectNear(failures, evMinusTwo, 0.03125f, "EV -2 patch value");
    ExpectNear(failures, evZero, 0.125f, "EV 0 patch value");
    ExpectNear(failures, evPlusTwo, 0.5f, "EV +2 patch value");
    ExpectNear(failures, evPlusTwo / evMinusTwo, 16.0f, "four-stop endpoint ratio");

    for (const float ev : {-2.0f, 0.0f, 2.0f})
    {
        const ReconstructionExposurePolicy policy = ResolveReconstructionExposurePolicy(ev);
        ExpectNear(failures, policy.reconstructionPreExposure, 1.0f,
            "reconstruction pre-exposure stays neutral");
        ExpectNear(failures, policy.reconstructionExposureScale, 1.0f,
            "reconstruction exposure scale stays neutral");
        ExpectNear(failures, policy.bloomExposureEv, ev,
            "bloom branch receives authored display EV");
        ExpectNear(failures, policy.displayExposureEv, ev,
            "tonemap branch receives authored display EV");

        const float base = ApplyDisplayExposure(kHdrPatch, policy.displayExposureEv);
        const float bloom = ApplyDisplayExposure(kHdrPatch, policy.bloomExposureEv);
        ExpectNear(failures, base, bloom, "base and bloom occupy the same exposed HDR space");
    }
}
