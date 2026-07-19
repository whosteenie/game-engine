#include <cstdint>
#include <iostream>

namespace
{
    // Mirrors assets/shaders/raytracing/common/hit_shading.hlsli PathRng (G3).
    struct PathRng
    {
        std::uint32_t seed = 1;
        std::uint32_t dimension = 0;
    };

    std::uint32_t Pcg3dHashX(const std::uint32_t x, const std::uint32_t y, const std::uint32_t z)
    {
        std::uint32_t vx = x * 1664525u + 1013904223u;
        std::uint32_t vy = y * 1664525u + 1013904223u;
        std::uint32_t vz = z * 1664525u + 1013904223u;
        vx += vy * vz;
        vy += vz * vx;
        vz += vx * vy;
        vx ^= vx >> 16u;
        vy ^= vy >> 16u;
        vz ^= vz >> 16u;
        vx += vy * vz;
        vy += vz * vx;
        vz += vx * vy;
        return vx;
    }

    PathRng InitPathRng(const std::uint32_t pixelX, const std::uint32_t pixelY, const std::uint32_t frameIndex)
    {
        PathRng rng;
        const std::uint32_t hx = Pcg3dHashX(pixelX, pixelY, frameIndex ^ 0xA341316Cu);
        rng.seed = hx != 0u ? hx : 1u;
        rng.dimension = 0u;
        return rng;
    }

    float PathRngNext(PathRng& rng)
    {
        const std::uint32_t h = Pcg3dHashX(rng.seed, rng.dimension, rng.seed ^ 0x9E3779B9u);
        ++rng.dimension;
        return float(h & 0x00FFFFFFu) * (1.0f / 16777216.0f);
    }

    void ExpectTrue(const bool condition, const char* message, int& failures)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << message << '\n';
            ++failures;
        }
    }
}

void RunPathRngTests(int& failures)
{
    const PathRng a = InitPathRng(10u, 20u, 3u);
    const PathRng b = InitPathRng(10u, 20u, 3u);
    ExpectTrue(a.seed == b.seed, "InitPathRng is deterministic for same pixel/frame", failures);

    PathRng streamA = a;
    PathRng streamB = b;
    float seq[8]{};
    for (int i = 0; i < 8; ++i)
    {
        seq[i] = PathRngNext(streamA);
        ExpectTrue(seq[i] == PathRngNext(streamB), "PathRng replay matches for same seed", failures);
        ExpectTrue(seq[i] >= 0.0f && seq[i] < 1.0f, "PathRngNext in [0,1)", failures);
    }

    const PathRng otherPixel = InitPathRng(11u, 20u, 3u);
    ExpectTrue(otherPixel.seed != a.seed, "Different pixels get different seeds", failures);

    PathRng replay = InitPathRng(10u, 20u, 3u);
    for (int i = 0; i < 8; ++i)
    {
        ExpectTrue(PathRngNext(replay) == seq[i], "Fresh InitPathRng replays the full stream", failures);
    }

    PathRng stream = InitPathRng(1u, 2u, 0u);
    const float x0 = PathRngNext(stream);
    const float x1 = PathRngNext(stream);
    ExpectTrue(x0 != x1, "Consecutive PathRng draws are not identical", failures);
    ExpectTrue(stream.dimension == 2u, "PathRng dimension advances by 1 per draw", failures);
}
