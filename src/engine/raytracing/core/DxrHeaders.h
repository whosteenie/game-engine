#pragma once

#include <d3d12.h>

#ifndef D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE
#define D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE \
    static_cast<D3D12_RESOURCE_STATES>(0x400000)
#endif

// AS build inputs (vertex/index buffers) must be readable as non-pixel-shader resources.
// The previous value here (0x800000) was NOT a valid build-input state — it collides with an
// unrelated D3D12 state bit and produced invalid transitions on every BLAS build (DXR-01).
#ifndef D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_INPUT
#define D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_INPUT \
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
#endif
