// ReSTIR GI sample / reservoir layouts (devdoc/dxr/pt/restir-pt.md §2, G8).
// Must match src/engine/raytracing/RestirTypes.h byte-for-byte.

#ifndef RESTIR_TYPES_HLSLI
#define RESTIR_TYPES_HLSLI

struct RestirInitialSample
{
    float3 xs;
    uint nsOct; // octahedral normal as two fp16
    uint loTailRg; // Y.rg as fp16 (R2: Y = t1·Lo_tail contribution)
    uint loTailBFlags; // Y.b fp16 in low 16, flags in high 16
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
