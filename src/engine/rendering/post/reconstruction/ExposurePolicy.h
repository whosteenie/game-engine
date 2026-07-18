#pragma once

#include <cmath>

// S2-P1 exposure ownership. Reconstruction operates on scene-linear HDR; authored display EV is
// applied only by bloom extraction and tonemapping after direct/DLSS/RR output. The renderer has no
// separate reconstruction pre-exposure signal today, so ordinary DLSS receives neutral guidance.
// RR exposure is intentionally omitted by DlssContext because the integrated RR contract does not
// support it.
struct ReconstructionExposurePolicy
{
    float reconstructionPreExposure = 1.0f;
    float reconstructionExposureScale = 1.0f;
    float bloomExposureEv = 0.0f;
    float displayExposureEv = 0.0f;
};

inline ReconstructionExposurePolicy ResolveReconstructionExposurePolicy(
    const float authoredDisplayEv)
{
    ReconstructionExposurePolicy policy{};
    policy.bloomExposureEv = authoredDisplayEv;
    policy.displayExposureEv = authoredDisplayEv;
    return policy;
}

inline float ApplyDisplayExposure(const float sceneLinearValue, const float authoredDisplayEv)
{
    return sceneLinearValue * std::exp2(authoredDisplayEv);
}
