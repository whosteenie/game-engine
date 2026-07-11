#ifndef MESHLET_CULL_HLSLI
#define MESHLET_CULL_HLSLI

// Shared GPU meshlet-culling math (C5.5). Used by the G-buffer amplification shader and, from C6,
// the shadow amplification shader — the only difference between the two is which frustum's planes
// are supplied (camera frustum for the G-buffer, cascade frustum for shadows).

// Conservative world-space bounding sphere of an object-space sphere under a possibly non-uniform
// world transform: the center transforms directly; the radius grows by the largest axis scale so the
// sphere never shrinks below the true (ellipsoidal) extent — culling stays conservative.
void MeshletWorldBoundingSphere(
    float3 objectCenter,
    float objectRadius,
    float4x4 world,
    out float3 worldCenter,
    out float worldRadius)
{
    worldCenter = mul(world, float4(objectCenter, 1.0)).xyz;
    const float sx = length(mul((float3x3)world, float3(1.0, 0.0, 0.0)));
    const float sy = length(mul((float3x3)world, float3(0.0, 1.0, 0.0)));
    const float sz = length(mul((float3x3)world, float3(0.0, 0.0, 1.0)));
    worldRadius = objectRadius * max(sx, max(sy, sz));
}

// planes: 6 world-space frustum planes, each (xyz = inward normal, w = offset) so a point p is inside
// when dot(plane.xyz, p) + plane.w >= 0. A sphere is culled only when it lies fully outside one plane
// (dist < -radius) — this never removes a meshlet that touches the frustum, so the raster result is
// identical to the un-culled path.
bool SphereInsideFrustum(float4 planes[6], float3 center, float radius)
{
    [unroll]
    for (uint i = 0u; i < 6u; ++i)
    {
        if (dot(planes[i].xyz, center) + planes[i].w < -radius)
        {
            return false;
        }
    }
    return true;
}

#endif // MESHLET_CULL_HLSLI
