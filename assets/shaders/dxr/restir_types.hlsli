// Production ReSTIR GI sample / reservoir layout (restir-production-roadmap.md P5).
// Must match src/engine/raytracing/RestirTypes.h byte-for-byte.

#ifndef RESTIR_TYPES_HLSLI
#define RESTIR_TYPES_HLSLI

struct RestirGiReservoir
{
    float3 position;
    uint normalOct;
    float3 radiance;
    float weightSum;
    uint M;
    uint age;
    uint flags;
    uint seed;
    uint instanceId;
    uint primitiveIndex;
    uint padding0;
    uint padding1;
};

// Layout retained only so the retired restir_stubs.hlsl experiment remains buildable until P8
// removes it. Active PT/DI passes never allocate or bind these source-contribution records.
struct RestirInitialSample
{
    float3 xs;
    uint nsOct;
    uint loTailRg;
    uint loTailBFlags;
    float pdf;
    uint seed;
};

struct RestirReservoir
{
    RestirInitialSample sample;
    float wSum;
    float W;
    uint M;
    uint age;
};

#endif // RESTIR_TYPES_HLSLI
