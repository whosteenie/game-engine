// PF5 compiler smoke test. This is intentionally not part of a production RTPSO: it proves that
// the modern library target accepts the inline-ray-query syntax needed by PF6, while startup still
// selects the legacy library on devices that cannot support it.

RaytracingAccelerationStructure g_Scene : register(t0);
RWTexture2D<uint> g_Result : register(u0);

[shader("raygeneration")]
void ModernRayQuerySmoke()
{
    RayDesc ray;
    ray.Origin = float3(0.0, 0.0, 0.0);
    ray.Direction = float3(0.0, 0.0, 1.0);
    ray.TMin = 0.001;
    ray.TMax = 1.0;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(g_Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xff, ray);
    while (query.Proceed())
    {
    }

    g_Result[uint2(0, 0)] = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 1u : 0u;
}
