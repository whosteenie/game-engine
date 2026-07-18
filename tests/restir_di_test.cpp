// ReSTIR DI initial-sampling (M=1) correctness gates (restir-production-roadmap.md P2).
// Proves, against the CPU mirror in RestirDi.h (identical math to restir_di.hlsli):
//   1. M=1 UCW identity: W == 1/p, so shade == f·V/p (byte-for-byte the plain one-sample NEE).
//   2. WRS proportionality: the selected candidate frequency matches its RIS weight share.
//   3. Unbiasedness / expected-value parity: the multi-candidate RIS estimator and the plain-NEE
//      estimator both converge to the same analytic direct-light integral (the P2 gate).

#include "engine/raytracing/restir/RestirDi.h"

#include <array>
#include <cmath>
#include <iostream>

namespace
{
    // Deterministic LCG (Numerical Recipes) → float in [0,1). Reproducible across runs/platforms.
    struct Lcg
    {
        std::uint32_t state;
        explicit Lcg(std::uint32_t seed) : state(seed) {}
        float Next()
        {
            state = state * 1664525u + 1013904223u;
            return static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
        }
    };

    // Toy 1D "direct light" on [0,1]: positive vector radiance g(x) and a non-uniform proposal p(x).
    // g is zero on part of the domain to also exercise zero-target candidates in the RIS normalizer.
    float Radiance(const float x)
    {
        return (x < 0.2f) ? 0.0f : (1.0f + 4.0f * x * x); // ∫_0.2^1 (1+4x²) dx, zero below 0.2
    }

    float ProposalPdf(const float x)
    {
        return 0.5f + x; // valid pdf on [0,1]: ∫ = 0.5 + 0.5 = 1, strictly positive
    }

    // Inverse-CDF sample of ProposalPdf: CDF(x) = 0.5x + x²/2 = u  →  x = -0.5 + sqrt(0.25 + 2u).
    float SampleProposal(const float u)
    {
        return -0.5f + std::sqrt(0.25f + 2.0f * u);
    }

    // Analytic ∫_0^1 g(x) dx = ∫_0.2^1 (1 + 4x²) dx = [x + 4x³/3]_0.2^1.
    float AnalyticIntegral()
    {
        auto antideriv = [](float x) { return x + 4.0f * x * x * x / 3.0f; };
        return antideriv(1.0f) - antideriv(0.2f);
    }

    void Vec3(float out[3], const float r)
    {
        out[0] = r;
        out[1] = 0.0f;
        out[2] = 0.0f;
    }
} // namespace

void RunRestirDiTests(int& failures)
{
    const float kDir[3] = {0.0f, 0.0f, 1.0f};

    // 1. M=1 UCW identity + per-sample NEE parity: W == 1/p and shade == f·V/p, exactly.
    {
        const float p = 0.37f;
        const float visibility = 0.5f;
        float f[3];
        Vec3(f, 3.0f);

        restir_di::Reservoir r = restir_di::Init();
        restir_di::Update(r, f, kDir, 5.0f, p, 0.5f);
        restir_di::Finalize(r);

        if (std::fabs(r.W - 1.0f / p) > 1e-5f)
        {
            std::cerr << "FAIL: ReSTIR DI M=1 UCW expected 1/p=" << (1.0f / p) << ", got " << r.W
                      << "\n";
            ++failures;
        }

        float shade[3];
        restir_di::Shade(r, visibility, shade);
        const float expected = f[0] * visibility / p; // plain one-sample NEE contribution
        if (std::fabs(shade[0] - expected) > 1e-4f)
        {
            std::cerr << "FAIL: ReSTIR DI M=1 shade expected f·V/p=" << expected << ", got "
                      << shade[0] << "\n";
            ++failures;
        }
    }

    // 1b. Zero-target candidate: still counts toward M, contributes nothing, W=0, shade=0.
    {
        float zero[3];
        Vec3(zero, 0.0f);
        restir_di::Reservoir r = restir_di::Init();
        restir_di::Update(r, zero, kDir, 1.0f, 0.5f, 0.5f);
        restir_di::Finalize(r);
        float shade[3];
        restir_di::Shade(r, 1.0f, shade);
        if (r.M != 1u || r.W != 0.0f || shade[0] != 0.0f)
        {
            std::cerr << "FAIL: ReSTIR DI zero-target expected M=1 W=0 shade=0, got M=" << r.M
                      << " W=" << r.W << " shade=" << shade[0] << "\n";
            ++failures;
        }
    }

    // 2. WRS proportionality: 3 fixed candidates → selection frequency ≈ RIS-weight share.
    {
        struct Cand
        {
            float contribution[3];
            float pdf;
        };
        std::array<Cand, 3> cands = {{
            {{2.0f, 0.0f, 0.0f}, 1.0f},
            {{6.0f, 0.0f, 0.0f}, 1.0f},
            {{1.0f, 0.0f, 0.0f}, 1.0f},
        }};
        // RIS weight w_i = lum(contribution)/pdf; luminance factor cancels in the share.
        std::array<double, 3> weight{};
        double weightSum = 0.0;
        for (int i = 0; i < 3; ++i)
        {
            weight[i] = restir_di::TargetLuminance(cands[i].contribution) / cands[i].pdf;
            weightSum += weight[i];
        }

        std::array<long, 3> selected{};
        const long trials = 2'000'000;
        Lcg rng(12345u);
        for (long t = 0; t < trials; ++t)
        {
            restir_di::Reservoir r = restir_di::Init();
            for (int i = 0; i < 3; ++i)
            {
                restir_di::Update(
                    r, cands[i].contribution, kDir, 1.0f, cands[i].pdf, rng.Next());
            }
            // Identify the winner by its stored contribution.r channel.
            int winner = 0;
            float best = 1e30f;
            for (int i = 0; i < 3; ++i)
            {
                const float d = std::fabs(r.contribution[0] - cands[i].contribution[0]);
                if (d < best)
                {
                    best = d;
                    winner = i;
                }
            }
            ++selected[winner];
        }

        for (int i = 0; i < 3; ++i)
        {
            const double observed = static_cast<double>(selected[i]) / trials;
            const double expected = weight[i] / weightSum;
            if (std::fabs(observed - expected) > 0.003)
            {
                std::cerr << "FAIL: ReSTIR DI WRS share[" << i << "] expected " << expected
                          << ", got " << observed << "\n";
                ++failures;
            }
        }
    }

    // 3. Unbiasedness / NEE parity (the P2 gate): RIS (M candidates) and plain NEE both converge to
    //    the analytic direct-light integral. Visibility = 1 here (target excludes it; §P2).
    {
        const long trials = 2'000'000;
        const int candidates = 4;
        const float analytic = AnalyticIntegral();

        double neeSum = 0.0;
        double risSum = 0.0;
        Lcg neeRng(777u);
        Lcg risRng(9001u);

        for (long t = 0; t < trials; ++t)
        {
            // Plain one-sample light NEE: x ~ p, estimate g(x)/p(x).
            const float xn = SampleProposal(neeRng.Next());
            neeSum += Radiance(xn) / ProposalPdf(xn);

            // RIS over `candidates` proposal draws, target p̂ = luminance(contribution).
            restir_di::Reservoir r = restir_di::Init();
            for (int c = 0; c < candidates; ++c)
            {
                const float xc = SampleProposal(risRng.Next());
                float f[3];
                Vec3(f, Radiance(xc));
                restir_di::Update(r, f, kDir, 1.0f, ProposalPdf(xc), risRng.Next());
            }
            restir_di::Finalize(r);
            float shade[3];
            restir_di::Shade(r, 1.0f, shade);
            risSum += shade[0];
        }

        const double neeMean = neeSum / trials;
        const double risMean = risSum / trials;

        if (std::fabs(neeMean - analytic) > 0.01)
        {
            std::cerr << "FAIL: plain-NEE mean " << neeMean << " != analytic " << analytic << "\n";
            ++failures;
        }
        if (std::fabs(risMean - analytic) > 0.01)
        {
            std::cerr << "FAIL: ReSTIR DI RIS mean " << risMean << " != analytic " << analytic
                      << " (expected-value parity gate)\n";
            ++failures;
        }
        if (std::fabs(risMean - neeMean) > 0.01)
        {
            std::cerr << "FAIL: ReSTIR DI RIS mean " << risMean << " diverges from NEE mean "
                      << neeMean << "\n";
            ++failures;
        }
    }

    // 4. P3 two-pixel temporal combine: source M is preserved and current-domain targets drive
    // selection. For scalar candidates 1 and 3, canonical reservoir combination evaluates to the
    // plain-MC mean 2 regardless of which candidate wins.
    {
        restir_di::Reservoir fresh = restir_di::Init();
        restir_di::Reservoir previous = restir_di::Init();
        const float one[3] = {1.0f, 1.0f, 1.0f};
        const float three[3] = {3.0f, 3.0f, 3.0f};
        restir_di::Update(fresh, one, kDir, 1.0f, 1.0f, 0.0f);
        restir_di::Update(previous, three, kDir, 1.0f, 1.0f, 0.0f);
        restir_di::Finalize(fresh);
        restir_di::Finalize(previous);

        for (const float xi : {0.1f, 0.9f})
        {
            restir_di::Reservoir combined = restir_di::Init();
            restir_di::CombineTemporal(combined, fresh, 1.0f, 0.0f);
            restir_di::CombineTemporal(combined, previous, 3.0f, xi);
            restir_di::CapAndFinalizeTemporal(combined);
            float shade[3];
            restir_di::Shade(combined, 1.0f, shade);
            if (combined.M != 2u || std::fabs(shade[0] - 2.0f) > 1e-5f)
            {
                std::cerr << "FAIL: P3 temporal combine expected M=2 and mean=2, got M="
                          << combined.M << " shade=" << shade[0] << "\n";
                ++failures;
            }
        }
    }

    // 5. P3 BASIC correction removes unsupported history energy. A history-selected sample that
    // has zero target/visibility at its source receiver must contribute zero instead of producing
    // an edge-energy gain through biased M normalization.
    {
        restir_di::Reservoir r = restir_di::Init();
        r.targetPdf = 4.0f;
        r.wSum = 8.0f;
        r.M = 2u;
        restir_di::FinalizeTemporalBasic(r, 4.0f, 0.0f, 1.0f, 1.0f, true);
        if (r.W != 0.0f)
        {
            std::cerr << "FAIL: P3 BASIC correction retained unsupported history energy\n";
            ++failures;
        }

        restir_di::FinalizeTemporalBasic(r, 4.0f, 2.0f, 1.0f, 1.0f, false);
        const float expected = 8.0f * 4.0f / (4.0f * (4.0f + 2.0f));
        if (std::fabs(r.W - expected) > 1e-6f)
        {
            std::cerr << "FAIL: P3 BASIC source-mixture normalization mismatch\n";
            ++failures;
        }
    }

    // 5b. Reservoir combination preserves the full candidate stream even when the source's
    // selected sample has zero target at the destination. Its RIS weight is zero, but its M still
    // participates in normalization; dropping it creates a positive energy bias.
    {
        restir_di::Reservoir source = restir_di::Init();
        const float one[3] = {1.0f, 1.0f, 1.0f};
        restir_di::Update(source, one, kDir, 1.0f, 1.0f, 0.0f);
        restir_di::Finalize(source);

        restir_di::Reservoir combined = restir_di::Init();
        restir_di::CombineTemporal(combined, source, 0.0f, 0.0f);
        if (combined.M != 1u || combined.wSum != 0.0f || combined.targetPdf != 0.0f)
        {
            std::cerr << "FAIL: zero-target reservoir source must preserve M without adding weight\n";
            ++failures;
        }
    }

    // 6. P4 multi-source correction uses the selected source in the numerator and every compatible
    // source's target*M in the denominator. This is the three-pixel extension of the P3 test.
    {
        restir_di::Reservoir r = restir_di::Init();
        r.targetPdf = 3.0f;
        r.wSum = 18.0f;
        r.M = 6u;
        const float sourceTargetTimesMSum = 3.0f * 1.0f + 2.0f * 2.0f + 1.0f * 3.0f;
        restir_di::FinalizeSpatialBasic(r, 2.0f, sourceTargetTimesMSum);
        const float expected = 18.0f * 2.0f / (3.0f * sourceTargetTimesMSum);
        if (std::fabs(r.W - expected) > 1e-6f)
        {
            std::cerr << "FAIL: P4 multi-source normalization mismatch\n";
            ++failures;
        }

        restir_di::FinalizeSpatialBasic(r, 0.0f, sourceTargetTimesMSum);
        if (r.W != 0.0f)
        {
            std::cerr << "FAIL: P4 unsupported selected source retained energy\n";
            ++failures;
        }
    }

}
