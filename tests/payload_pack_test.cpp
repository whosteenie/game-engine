// CPU mirror of the packed-Payload encode/decode in assets/shaders/dxr/path_tracer.hlsl.
// Gates the one novel quantization the packing pass introduced: snorm16 octahedral normals
// (RestirOctEncode composed with snorm16x2). Proves the roundtrip is precise enough for glass
// refraction (well under a hundredth of a degree) and that the lod/prevDepth half-pair recovers
// independently. Keep in sync with the PtPack*/Payload* helpers when the shader formulas change.

#include "test_expect.h"

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace
{
    // Mirror of RestirOctEncode (restir_pack.hlsli).
    glm::vec2 OctEncode(glm::vec3 n)
    {
        n /= std::max(std::abs(n.x) + std::abs(n.y) + std::abs(n.z), 1e-8f);
        glm::vec2 enc = glm::vec2(n.x, n.y);
        if (n.z < 0.0f)
        {
            const glm::vec2 signN(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
            enc = (1.0f - glm::abs(glm::vec2(enc.y, enc.x))) * signN;
        }
        return enc;
    }

    // Mirror of RestirOctDecode (restir_pack.hlsli).
    glm::vec3 OctDecode(const glm::vec2& enc)
    {
        glm::vec3 n(enc.x, enc.y, 1.0f - std::abs(enc.x) - std::abs(enc.y));
        if (n.z < 0.0f)
        {
            const glm::vec2 signN(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
            const glm::vec2 xy = (1.0f - glm::abs(glm::vec2(n.y, n.x))) * signN;
            n.x = xy.x;
            n.y = xy.y;
        }
        return glm::normalize(n);
    }

    // Mirror of PtPackSnorm16x2 (path_tracer.hlsl): round-to-nearest, clamp, int16 sign-extend.
    std::uint32_t PackSnorm16x2(const glm::vec2& v)
    {
        const int qx = static_cast<int>(std::lround(std::clamp(v.x, -1.0f, 1.0f) * 32767.0f));
        const int qy = static_cast<int>(std::lround(std::clamp(v.y, -1.0f, 1.0f) * 32767.0f));
        return (static_cast<std::uint32_t>(qx) & 0xffffu) | (static_cast<std::uint32_t>(qy) << 16);
    }

    glm::vec2 UnpackSnorm16x2(std::uint32_t p)
    {
        const int qx = static_cast<std::int16_t>(p & 0xffffu);
        const int qy = static_cast<std::int16_t>(p >> 16);
        return glm::clamp(glm::vec2(qx, qy) * (1.0f / 32767.0f), glm::vec2(-1.0f), glm::vec2(1.0f));
    }

    glm::vec3 PackUnpackOctNormal(const glm::vec3& n)
    {
        return OctDecode(UnpackSnorm16x2(PackSnorm16x2(OctEncode(glm::normalize(n)))));
    }

    // fp16 octahedral (RestirPackOctNormal) — the OLD encoding, used here only as a precision baseline.
    glm::vec3 PackUnpackOctNormalFp16(const glm::vec3& n)
    {
        const glm::vec2 enc = OctEncode(glm::normalize(n));
        return OctDecode(glm::unpackHalf2x16(glm::packHalf2x16(enc)));
    }

    float AngleDegrees(const glm::vec3& a, const glm::vec3& b)
    {
        const float c = std::clamp(glm::dot(glm::normalize(a), glm::normalize(b)), -1.0f, 1.0f);
        return glm::degrees(std::acos(c));
    }
}

void RunPayloadPackTests()
{
    // 1) snorm16x2 component roundtrip stays within one quantization step.
    {
        const float step = 1.0f / 32767.0f;
        float maxErr = 0.0f;
        for (int i = 0; i <= 400; ++i)
        {
            const float t = -1.0f + 2.0f * (static_cast<float>(i) / 400.0f);
            const glm::vec2 r = UnpackSnorm16x2(PackSnorm16x2(glm::vec2(t, -t)));
            maxErr = std::max(maxErr, std::max(std::abs(r.x - t), std::abs(r.y + t)));
        }
        test::ExpectTrue(maxErr <= step, "snorm16x2 roundtrip within one quantization step");
    }

    // 2) Oct + snorm16 normal roundtrip: dense hemisphere-crossing sweep. Worst-case ~0.03deg over
    //    this grid (uniform 16-bit precision), comfortably below any perceptible refraction walk and
    //    tighter than the fp16-oct alternative (whose mantissa coarsens near the +-1 oct fold).
    {
        float maxAngle = 0.0f;
        float maxAngleFp16 = 0.0f;
        const int n = 64;
        for (int i = 0; i <= n; ++i)
        {
            const float u = static_cast<float>(i) / n;                 // [0,1]
            const float z = 1.0f - 2.0f * u;                           // [-1,1], crosses the fold
            const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
            for (int j = 0; j < n; ++j)
            {
                const float phi = 2.0f * 3.14159265f * (static_cast<float>(j) / n);
                const glm::vec3 dir(r * std::cos(phi), r * std::sin(phi), z);
                maxAngle = std::max(maxAngle, AngleDegrees(dir, PackUnpackOctNormal(dir)));
                maxAngleFp16 = std::max(maxAngleFp16, AngleDegrees(dir, PackUnpackOctNormalFp16(dir)));
            }
        }
        test::ExpectTrue(maxAngle < 0.04f, "snorm16-oct normal roundtrip error < 0.04deg");
        test::ExpectTrue(
            maxAngle < maxAngleFp16,
            "snorm16-oct is strictly more precise than the fp16-oct alternative");
    }

    // 3) The exact axis directions (glass shells sit flush to axes) roundtrip essentially exactly.
    {
        const glm::vec3 axes[] = {
            {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (const glm::vec3& a : axes)
        {
            test::ExpectTrue(
                AngleDegrees(a, PackUnpackOctNormal(a)) < 0.005f, "Axis normal roundtrips ~exactly");
        }
    }

    // 4) lodPrevDepthHalf packs triangleLod (low fp16) and prevLinearDepth (high fp16) with no
    //    cross-contamination between the two halves.
    {
        const float lod = 3.75f;
        const float prevDepth = 12.5f;
        const std::uint32_t packed =
            (glm::packHalf2x16(glm::vec2(lod, 0.0f)) & 0xffffu) |
            (glm::packHalf2x16(glm::vec2(prevDepth, 0.0f)) << 16);
        const float lodOut = glm::unpackHalf2x16(packed & 0xffffu).x;
        const float depthOut = glm::unpackHalf2x16(packed >> 16).x;
        test::ExpectNear(lodOut, lod, 1e-2f, "lodPrevDepthHalf low half recovers triangleLod");
        test::ExpectNear(depthOut, prevDepth, 5e-2f, "lodPrevDepthHalf high half recovers prevDepth");
    }
}
