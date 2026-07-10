// G8 ReSTIR temporal/spatial raygen stubs (devdoc/dxr/pt/restir-pt.md).
// Pipelines + SBTs warm up; no DispatchRays until R2/R3. Buffers are declared so the root
// signature matches the eventual reuse passes.

#include "restir_types.hlsli"

cbuffer DispatchConstants : register(b0)
{
    uint2 g_OutputSize;
    uint2 _Padding0;
};

RaytracingAccelerationStructure g_SceneTlas : register(t0);
RWStructuredBuffer<RestirReservoir> g_ReservoirCurrent : register(u0);
RWStructuredBuffer<RestirReservoir> g_ReservoirPrev : register(u1);
RWStructuredBuffer<RestirInitialSample> g_InitialSample : register(u2);

struct Payload
{
    float unused;
};

[shader("raygeneration")]
void RestirTemporalRayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    if (pixel.x >= g_OutputSize.x || pixel.y >= g_OutputSize.y)
    {
        return;
    }

    // G8 stub — no reservoir writes until R2.
    (void)g_SceneTlas;
    (void)g_ReservoirCurrent;
    (void)g_ReservoirPrev;
    (void)g_InitialSample;
}

[shader("raygeneration")]
void RestirSpatialRayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    if (pixel.x >= g_OutputSize.x || pixel.y >= g_OutputSize.y)
    {
        return;
    }

    // G8 stub — no reservoir writes until R3.
    (void)g_SceneTlas;
    (void)g_ReservoirCurrent;
    (void)g_ReservoirPrev;
    (void)g_InitialSample;
}

[shader("miss")]
void RestirMiss(inout Payload payload)
{
    payload.unused = 0.0f;
}

[shader("closesthit")]
void RestirClosestHit(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    payload.unused = 0.0f;
    (void)attribs;
}
