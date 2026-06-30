#pragma once

#include "engine/raytracing/DxrShaderCache.h"
#include "engine/rendering/ShaderCache.h"

namespace RenderingPipelineCache
{
    inline void InvalidateAll()
    {
        ShaderCache::Clear();
        DxrShaderCache::Clear();
    }
}
