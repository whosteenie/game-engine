// Monte-Carlo simulation of the RECURSIVE temporal-reuse chain from restir_di_temporal.hlsl
// (GiTemporalResample / ResampleDomain), in the static-camera / same-domain regime where the GPU
// exhibits a constant hard-line bias that survives reference-mode accumulation.
//
// The single temporal STEP is provably unbiased (restir_gi_test.cpp multi-domain case). This test
// asks the different question the GPU result forces: does the FIXED POINT of iterating that step —
// fresh(M=1) combined with the pixel's own evolving history every frame, M-capped — still estimate
// the true integral in expectation? If the measured mean drifts from ground truth, the recursion is
// the bug and we can bisect it here. If it matches, the bias is GPU-only (compositing/reprojection).

#include "test_expect.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>

namespace
{
// Discrete two-direction "hemisphere". Each frame the fresh sampler draws direction k with
// probability prob[k]; that direction's reconnected contribution is contrib[k] (bsdf*cos*radiance)
// and its RIS target equals that contribution (scalar proxy for luminance(f*L)). The one-sample MC
// estimator f*L/pdf is unbiased, so the ground-truth integral is sum_k contrib[k].
struct Scene
{
    static constexpr int kDirs = 2;
    double prob[kDirs] = {0.5, 0.5};
    double contrib[kDirs] = {0.8, 0.6}; // A brighter target than B -> RIS prefers A (bias stress)
    double truth() const { return contrib[0] + contrib[1]; } // = 1.4
};

// Mirror of the GI reservoir's reusable state across frames.
struct Reservoir
{
    bool has = false;
    int idx = 0;
    double W = 0.0;
    double M = 0.0;
};

constexpr double kMCap = 20.0; // kRestirMCap

// One frame of GiTemporalResample in the same-domain (static) regime. Returns the shaded estimate
// for this frame and updates `res` to the new reservoir, exactly as the shader does.
double StepChain(const Scene& s, Reservoir& res, std::mt19937& rng)
{
    std::uniform_real_distribution<double> uni(0.0, 1.0);

    // Fresh candidate (P5): sample a direction, initial UCW = 1/pdf, M = 1.
    const int k = uni(rng) < s.prob[0] ? 0 : 1;
    const double freshTarget = s.contrib[k];
    const double freshW = 1.0 / s.prob[k];
    const double freshRis = freshTarget * freshW * 1.0;

    double wSum = freshRis;
    int selIdx = k;
    double previousM = 0.0;

    // Combine the reprojected history (same domain: target evaluated at current == at previous).
    if (res.has)
    {
        const double mh = std::min(res.M, kMCap);       // previous.M = min(previous.M, kRestirMCap)
        const double prevTarget = s.contrib[res.idx];
        const double prevRis = prevTarget * res.W * mh; // targetAtCurrent * sourceUcw * sourceM
        wSum += prevRis;
        previousM = mh;
        if (uni(rng) * wSum <= prevRis)                 // WRS: history replaces fresh
        {
            selIdx = res.idx;
        }
    }

    const double outM = 1.0 + previousM;                // fresh.M + capped previous.M
    const double selTarget = s.contrib[selIdx];         // same in both domains (static)

    // RTXDI BASIC finalize (FinalizeTemporalBasic with selTargetCurrent == selTargetPrevious).
    const double piSum = selTarget * 1.0 + selTarget * previousM; // selTarget*(1+previousM)
    const double denom = piSum * selTarget;
    const double outW = denom > 0.0 ? wSum * selTarget / denom : 0.0;

    res.has = true;
    res.idx = selIdx;
    res.W = outW;
    res.M = outM;

    // ShadeGi: contribution * UCW (== selTarget * outW for this scalar proxy).
    return selTarget * outW;
}

// Same recursive chain, but each frame the per-direction contribution/target is perturbed by a
// mean-1 multiplicative jitter (models reference-mode sub-pixel jitter reevaluating the reused
// sample's bsdf*cos*radiance every frame). Fresh alone stays unbiased (E[jitter]=1); the question
// is whether the RECURSIVE reuse — which divides accumulated wSum by the CURRENT jittered target —
// develops a ratio bias. Both the RIS target and the final shade use the same jittered value.
double StepChainJitter(const Scene& s, Reservoir& res, std::mt19937& rng, double jitterAmp)
{
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    auto jit = [&]() { return 1.0 + jitterAmp * (2.0 * uni(rng) - 1.0); }; // mean 1
    double eff[Scene::kDirs];
    for (int i = 0; i < Scene::kDirs; ++i) eff[i] = s.contrib[i] * jit();

    const int k = uni(rng) < s.prob[0] ? 0 : 1;
    const double freshW = 1.0 / s.prob[k];
    double wSum = eff[k] * freshW;
    int selIdx = k;
    double previousM = 0.0;
    if (res.has)
    {
        const double mh = std::min(res.M, kMCap);
        const double prevRis = eff[res.idx] * res.W * mh; // history reevaluated with CURRENT jitter
        wSum += prevRis;
        previousM = mh;
        if (uni(rng) * wSum <= prevRis) selIdx = res.idx;
    }
    const double selTarget = eff[selIdx];
    const double piSum = selTarget * (1.0 + previousM);
    const double denom = piSum * selTarget;
    const double outW = denom > 0.0 ? wSum * selTarget / denom : 0.0;
    res.has = true;
    res.idx = selIdx;
    res.W = outW;
    res.M = 1.0 + previousM;
    return selTarget * outW;
}

// Like StepChainJitter, but faithfully models the RTXDI BASIC bias correction: the selected sample's
// target at the PREVIOUS-frame domain (`temporalP`/`selectedPreviousTarget`) is evaluated with the
// PREVIOUS frame's jitter (`effPrev`), while selection/shade use the CURRENT frame's jitter. This
// current-vs-previous mismatch in the UCW, fed back recursively, is the last untested combine path
// shared by DI and GI. `effPrev` carries the previous frame's per-direction effective targets.
double StepChainBasicJitter(
    const Scene& s, Reservoir& res, std::mt19937& rng, double jitterAmp, double effPrev[Scene::kDirs])
{
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    auto jit = [&]() { return 1.0 + jitterAmp * (2.0 * uni(rng) - 1.0); };
    double eff[Scene::kDirs];
    for (int i = 0; i < Scene::kDirs; ++i) eff[i] = s.contrib[i] * jit();

    const int k = uni(rng) < s.prob[0] ? 0 : 1;
    const double freshW = 1.0 / s.prob[k];
    double wSum = eff[k] * freshW;
    int selIdx = k;
    bool selectedPrevious = false;
    double previousM = 0.0;
    if (res.has)
    {
        const double mh = std::min(res.M, kMCap);
        const double prevRis = eff[res.idx] * res.W * mh; // combine target: CURRENT domain
        wSum += prevRis;
        previousM = mh;
        if (uni(rng) * wSum <= prevRis) { selIdx = res.idx; selectedPrevious = true; }
    }
    const double selTargetCurrent = eff[selIdx];
    const double selTargetPrevious = effPrev[selIdx];              // selected sample @ previous domain
    const double pi = selectedPrevious ? selTargetPrevious : selTargetCurrent;
    const double piSum = selTargetCurrent * 1.0 + selTargetPrevious * previousM;
    const double denom = piSum * selTargetCurrent;
    const double outW = denom > 0.0 ? wSum * pi / denom : 0.0;
    res.has = true;
    res.idx = selIdx;
    res.W = outW;
    res.M = 1.0 + previousM;
    for (int i = 0; i < Scene::kDirs; ++i) effPrev[i] = eff[i];    // this frame becomes prev
    return selTargetCurrent * outW;
}

double MeasureBasicJitterChain(double jitterAmp)
{
    const Scene scene;
    std::mt19937 rng(0x0BA51C99u);
    double sum = 0.0;
    std::int64_t count = 0;
    for (int p = 0; p < 8000; ++p)
    {
        Reservoir res;
        double effPrev[Scene::kDirs] = {scene.contrib[0], scene.contrib[1]};
        for (int f = 0; f < 400; ++f)
        {
            const double shade = StepChainBasicJitter(scene, res, rng, jitterAmp, effPrev);
            if (f >= 200) { sum += shade; ++count; }
        }
    }
    return sum / static_cast<double>(count);
}

double MeasureJitterChain(double jitterAmp)
{
    const Scene scene;
    std::mt19937 rng(0xB19A5EEDu);
    double sum = 0.0;
    std::int64_t count = 0;
    for (int p = 0; p < 8000; ++p)
    {
        Reservoir res;
        for (int f = 0; f < 400; ++f)
        {
            const double shade = StepChainJitter(scene, res, rng, jitterAmp);
            if (f >= 200) { sum += shade; ++count; }
        }
    }
    return sum / static_cast<double>(count);
}
} // namespace

// Same chain, but with the production visibility split: the RIS target is the UNSHADOWED
// contribution (no V), while the shade multiplies by V for the selected sample (exactly ShadeDomain:
// f * W * Visibility, EvaluateSample's target excludes V). Direction A has the highest unshadowed
// target but is fully occluded (V=0). The one-sample estimator f*L/pdf*V is still unbiased, so truth
// is the SHADOWED integral sum_k contrib[k]*vis[k].
struct VisScene
{
    static constexpr int kDirs = 3;
    double prob[kDirs] = {1.0 / 3, 1.0 / 3, 1.0 / 3};
    double contrib[kDirs] = {0.9, 0.5, 0.3}; // A is the brightest unshadowed target...
    double vis[kDirs] = {0.0, 1.0, 1.0};     // ...but A is occluded, so it truly contributes nothing.
    double truth() const
    {
        return contrib[0] * vis[0] + contrib[1] * vis[1] + contrib[2] * vis[2]; // = 0.8
    }
};

// One frame with the visibility split. `visibleTarget`=false reproduces the PRODUCTION code (RIS
// target is the unshadowed contribution; visibility applied only at shade). `visibleTarget`=true is
// the candidate FIX (visibility folded into the target so occluded samples cannot win/inflate).
double StepChainVis(const VisScene& s, Reservoir& res, std::mt19937& rng, bool visibleTarget)
{
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    const double r0 = uni(rng);
    const int k = r0 < s.prob[0] ? 0 : (r0 < s.prob[0] + s.prob[1] ? 1 : 2);
    auto target = [&](int i) { return visibleTarget ? s.contrib[i] * s.vis[i] : s.contrib[i]; };

    const double freshW = 1.0 / s.prob[k];
    double wSum = target(k) * freshW;
    int selIdx = k;
    double previousM = 0.0;
    if (res.has)
    {
        const double mh = std::min(res.M, kMCap);
        const double prevRis = target(res.idx) * res.W * mh;
        wSum += prevRis;
        previousM = mh;
        if (uni(rng) * wSum <= prevRis)
        {
            selIdx = res.idx;
        }
    }
    const double outM = 1.0 + previousM;
    const double selTarget = target(selIdx);
    const double piSum = selTarget * (1.0 + previousM);
    const double denom = piSum * selTarget;
    const double outW = denom > 0.0 ? wSum * selTarget / denom : 0.0;
    res.has = true;
    res.idx = selIdx;
    res.W = outW;
    res.M = outM;
    // Shade always applies true visibility to the selected sample (ShadeDomain: f * W * V).
    return s.contrib[selIdx] * outW * s.vis[selIdx];
}

double MeasureVisChain(bool visibleTarget)
{
    const VisScene scene;
    std::mt19937 rng(0x5EED1234u);
    const int kPixels = 4000;
    const int kFrames = 400;
    const int kBurnIn = 200;
    double sum = 0.0;
    std::int64_t count = 0;
    for (int p = 0; p < kPixels; ++p)
    {
        Reservoir res;
        for (int f = 0; f < kFrames; ++f)
        {
            const double shade = StepChainVis(scene, res, rng, visibleTarget);
            if (f >= kBurnIn)
            {
                sum += shade;
                ++count;
            }
        }
    }
    return sum / static_cast<double>(count);
}

void RunVisibilitySplitChain(int& failures)
{
    const auto expect = [&](const bool cond, const char* msg) {
        if (!cond)
        {
            std::fprintf(stderr, "FAIL: %s\n", msg);
            ++failures;
        }
    };
    const double truth = VisScene{}.truth();

    // Production behavior: visibility OUT of the target -> occluded high-target sample inflates the
    // weight of visible winners -> converged image is systematically too BRIGHT. This documents the
    // root cause of the constant P6 hard line; it must be a clearly super-MC-noise brightening.
    const double biasedMean = MeasureVisChain(/*visibleTarget=*/false);
    const double biasedErr = (biasedMean - truth) / truth;
    std::fprintf(stderr, "[temporal_chain/vis unshadowed-target] truth=%.5f mean=%.5f relError=%+.4f%%\n",
        truth, biasedMean, 100.0 * biasedErr);
    expect(biasedErr > 0.01,
        "Visibility-outside-target must reproduce the recursive brightening bias (root cause)");

    // Candidate fix: visibility IN the target -> occluded samples never win or inflate -> unbiased.
    const double fixedMean = MeasureVisChain(/*visibleTarget=*/true);
    const double fixedErr = (fixedMean - truth) / truth;
    std::fprintf(stderr, "[temporal_chain/vis shadowed-target]   truth=%.5f mean=%.5f relError=%+.4f%%\n",
        truth, fixedMean, 100.0 * fixedErr);
    expect(std::abs(fixedErr) < 0.005,
        "Folding visibility into the RIS target removes the recursive brightening bias");
}

// GI reconnection-Jacobian regime. A single fixed secondary is reused every frame; the primary is
// re-jittered each frame (sub-pixel). The GI temporal code multiplies the history UCW by
// GiTemporalJacobian(previous, previousPrimary, currentPrimary) — with previous/current being the
// per-frame JITTERED hit points even for a static surface. jacobianOn=false forces jacobian=1.
struct V3 { double x, y, z; };
static double Dot3(V3 a, V3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static double Len3(V3 a) { return std::sqrt(Dot3(a, a)); }
static V3 Sub3(V3 a, V3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }

static double GiJacobian(V3 secPos, V3 secN, V3 prevPrimary, V3 currPrimary)
{
    const V3 toPrev = Sub3(prevPrimary, secPos);
    const V3 toCurr = Sub3(currPrimary, secPos);
    const double prevDist = Len3(toPrev), currDist = Len3(toCurr);
    if (prevDist <= 1e-4 || currDist <= 1e-4) return 0.0;
    const double prevCos = Dot3(secN, toPrev) / prevDist;
    const double currCos = Dot3(secN, toCurr) / currDist;
    if (prevCos <= 1e-4 || currCos <= 1e-4) return 0.0;
    const double jac = (currCos / prevCos) * ((prevDist*prevDist) / (currDist*currDist));
    return (std::isfinite(jac) && jac >= 1.0/16.0 && jac <= 4.0) ? jac : 0.0;
}

double MeasureJacobianChain(bool jacobianOn, double jitterAmp, double& outMeanJac)
{
    const V3 S{0.0, 0.0, 0.0};
    const double n = std::sqrt(0.4*0.4 + 1.0);
    const V3 secN{0.4/n, 0.0, 1.0/n};
    const V3 P0{0.5, 0.0, 1.5};                 // primary mean (grazing-ish to secN)
    const double c = 1.0;                        // constant target (isolates the Jacobian)
    const double freshW = 1.0;                   // fresh UCW; true shade = c*freshW = 1
    std::mt19937 rng(0x3AC0B123u);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    auto jitter = [&]() -> V3 {
        return {jitterAmp*(2*uni(rng)-1), jitterAmp*(2*uni(rng)-1), jitterAmp*(2*uni(rng)-1)};
    };
    double sum = 0.0, jacSum = 0.0; std::int64_t count = 0, jacCount = 0;
    for (int p = 0; p < 6000; ++p)
    {
        Reservoir res;
        V3 prevPrimary = P0;
        for (int f = 0; f < 400; ++f)
        {
            const V3 j = jitter();
            const V3 curr{P0.x + j.x, P0.y + j.y, P0.z + j.z};
            double wSum = c * freshW; double previousM = 0.0;
            if (res.has)
            {
                double jac = jacobianOn ? GiJacobian(S, secN, prevPrimary, curr) : 1.0;
                if (jac > 0.0)
                {
                    jacSum += jac; ++jacCount;
                    const double mh = std::min(res.M, kMCap);
                    const double risHist = c * (res.W * jac) * mh;
                    wSum += risHist;
                    previousM = mh;
                    (void)uni(rng); // consume RNG symmetry with real WRS; selection value unused (same sample)
                }
            }
            const double outW = wSum / (c * (1.0 + previousM));
            res.has = true; res.W = outW; res.M = 1.0 + previousM;
            prevPrimary = curr;
            if (f >= 200) { sum += c * outW; ++count; }
        }
    }
    outMeanJac = jacCount ? jacSum / static_cast<double>(jacCount) : 1.0;
    return sum / static_cast<double>(count);
}

void RunRestirTemporalChainTests(int& failures)
{
    const auto expect = [&](const bool cond, const char* msg) {
        if (!cond)
        {
            std::fprintf(stderr, "FAIL: %s\n", msg);
            ++failures;
        }
    };

    const Scene scene;
    const double truth = scene.truth();

    // Average the shaded estimate over the converged chain across many independent pixels/seeds.
    std::mt19937 rng(0xC0FFEEu);
    const int kPixels = 4000;
    const int kFrames = 400;   // let the reservoir reach its M-capped fixed point
    const int kBurnIn = 200;   // discard the transient
    double sum = 0.0;
    std::int64_t count = 0;
    for (int p = 0; p < kPixels; ++p)
    {
        Reservoir res;
        for (int f = 0; f < kFrames; ++f)
        {
            const double shade = StepChain(scene, res, rng);
            if (f >= kBurnIn)
            {
                sum += shade;
                ++count;
            }
        }
    }
    const double mean = sum / static_cast<double>(count);
    const double relError = (mean - truth) / truth;
    std::fprintf(
        stderr,
        "[temporal_chain] truth=%.5f converged mean=%.5f relError=%+.4f%%\n",
        truth,
        mean,
        100.0 * relError);

    // MC standard error over 1.6M samples is well under 0.5%; a real recursive bias shows as a
    // systematic offset far exceeding that. Flag anything above 1%.
    expect(std::abs(relError) < 0.01,
        "Recursive temporal-reuse chain must remain unbiased in the static/same-domain fixed point");

    // Jitter regime (models reference-mode sub-pixel jitter reevaluating the reused target each
    // frame). If the recursive chain drifts here it explains a bias that survives reference
    // accumulation yet is absent from the fixed-target sim above.
    for (double amp : {0.15, 0.3, 0.5})
    {
        const double jMean = MeasureJitterChain(amp);
        const double jErr = (jMean - truth) / truth;
        std::fprintf(stderr, "[temporal_chain/jitter amp=%.2f] truth=%.5f mean=%.5f relError=%+.4f%%\n",
            amp, truth, jMean, 100.0 * jErr);
    }
    for (double amp : {0.15, 0.3, 0.5})
    {
        const double bMean = MeasureBasicJitterChain(amp);
        const double bErr = (bMean - truth) / truth;
        std::fprintf(stderr, "[temporal_chain/basic-prevdomain amp=%.2f] truth=%.5f mean=%.5f relError=%+.4f%%\n",
            amp, truth, bMean, 100.0 * bErr);
    }

    // GI reconnection-Jacobian under jitter: THE suspect for the GI-only line. Static surface, only
    // sub-pixel jitter, single reused secondary. If jacobian-on brightens while jacobian=1 stays at
    // truth (1.0), the per-frame Jacobian on jittered primaries is the bias.
    for (double amp : {0.05, 0.1, 0.2})
    {
        double meanJacOn = 0.0, meanJac1 = 0.0;
        const double on = MeasureJacobianChain(true, amp, meanJacOn);
        const double off = MeasureJacobianChain(false, amp, meanJac1);
        std::fprintf(stderr,
            "[temporal_chain/jacobian amp=%.2f] jac=ON mean=%.5f (E[jac]=%.4f) | jac=1 mean=%.5f | truth=1.0\n",
            amp, on, meanJacOn, off);
    }

    RunVisibilitySplitChain(failures);
}
