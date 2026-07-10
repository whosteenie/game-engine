#pragma once

#include <cstdint>

// CPU mirror of assets/shaders/dxr/restir_types.hlsli (G8 / restir-pt.md §2).
// Structured-buffer strides must stay in lockstep with the HLSL structs.

struct RestirInitialSample
{
    float xs[3];
    std::uint32_t nsOct = 0; // octahedral normal as two fp16
    std::uint32_t loTailRg = 0; // Lo_tail.rg as fp16
    std::uint32_t loTailBFlags = 0; // Lo_tail.b fp16 in low 16, flags in high 16
    float pdf = 0.0f;
    std::uint32_t seed = 0;
};

static_assert(sizeof(RestirInitialSample) == 32, "RestirInitialSample must be 32 bytes");

struct RestirReservoir
{
    RestirInitialSample sample{};
    float wSum = 0.0f;
    float W = 0.0f;
    std::uint32_t M = 0; // confidence / sample count (doc allows fp16 packing; uint is fine)
    std::uint32_t pad = 0;
};

static_assert(sizeof(RestirReservoir) == 48, "RestirReservoir must be 48 bytes");
