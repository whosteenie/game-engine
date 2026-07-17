cbuffer DispatchConstants : register(b0)
{
    uint2 g_OutputSize;
    uint2 _Padding0;
    float4 g_ClearColor;
};

RWTexture2D<float4> g_Output : register(u0);
RaytracingAccelerationStructure g_SceneTlas : register(t0);

struct Payload
{
    float3 color;
};

[shader("raygeneration")]
void SmokeRayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    if (pixel.x >= g_OutputSize.x || pixel.y >= g_OutputSize.y)
    {
        return;
    }

    g_Output[pixel] = g_ClearColor;
}

[shader("miss")]
void SmokeMiss(inout Payload payload)
{
    payload.color = float3(1.0, 0.0, 1.0);
}

[shader("closesthit")]
void SmokeHit(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    payload.color = float3(0.0, 1.0, 0.0);
    (void)attribs;
}
