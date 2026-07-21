#pragma once

#include <cstdint>

// CPU mirrors of structured-buffer layouts in assets/shaders/raytracing/path_tracing.
struct RestirGiReservoir
{
    float position[3] = {};
    std::uint32_t normalOct = 0;
    float radiance[3] = {};
    float weightSum = 0.0f;
    std::uint32_t M = 0;
    std::uint32_t age = 0;
    std::uint32_t flags = 0;
    std::uint32_t seed = 0;
    std::uint32_t instanceId = 0;
    std::uint32_t primitiveIndex = 0;
    std::uint32_t padding[2] = {};
    float initialPosition[3] = {};
    std::uint32_t initialNormalOct = 0;
    float initialRadiance[3] = {};
    float initialWeightSum = 0.0f;
};

static_assert(sizeof(RestirGiReservoir) == 96, "RestirGiReservoir must be 96 bytes");

// Retired experiment mirrors, kept only for its isolated CPU regression fixture until P8 cleanup.
struct RestirInitialSample
{
    float xs[3] = {};
    std::uint32_t nsOct = 0;
    std::uint32_t loTailRg = 0;
    std::uint32_t loTailBFlags = 0;
    float pdf = 0.0f;
    std::uint32_t seed = 0;
};

struct RestirReservoir
{
    RestirInitialSample sample{};
    float wSum = 0.0f;
    float W = 0.0f;
    std::uint32_t M = 0;
    std::uint32_t age = 0;
};

static_assert(sizeof(RestirInitialSample) == 32);
static_assert(sizeof(RestirReservoir) == 48);

struct RestirDiLightSample
{
    std::uint32_t sampleType = 0;
    std::uint32_t index0 = 0;
    std::uint32_t index1 = 0;
    std::uint32_t padding0 = 0;
    float uv[2] = {};
    float padding1[2] = {};
};

struct RestirDiTemporalReservoir
{
    RestirDiLightSample sample{};
    float wSum = 0.0f;
    float targetPdf = 0.0f;
    float W = 0.0f;
    std::uint32_t M = 0;
    std::uint32_t age = 0;
    std::uint32_t padding[3] = {};
};

struct RestirDiReservoirSet
{
    RestirDiTemporalReservoir emissive{};
    RestirDiTemporalReservoir environment{};
};

static_assert(sizeof(RestirDiLightSample) == 32);
static_assert(sizeof(RestirDiTemporalReservoir) == 64);
static_assert(sizeof(RestirDiReservoirSet) == 128);
