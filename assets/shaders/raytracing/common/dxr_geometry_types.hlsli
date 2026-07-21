// Shared DXR geometry types (HK-E2).

#ifndef DXR_GEOMETRY_TYPES_HLSLI
#define DXR_GEOMETRY_TYPES_HLSLI

struct GeometryLookupEntry
{
    uint vertexFloatOffset;
    uint vertexStrideFloats;
    uint indexUintOffset;
    uint materialId;
};

static const uint kHitKindTriangleBackFace = 255u;

#endif // DXR_GEOMETRY_TYPES_HLSLI
