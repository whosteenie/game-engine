#pragma once

#include "engine/raytracing/DxrShaderCache.h"
#include "engine/rendering/ShaderCache.h"
#include "engine/rhi/d3d12/HlslCompiler.h"

namespace RenderingPipelineCache
{
    inline void InvalidateAll()
    {
        ShaderCache::Clear();
        DxrShaderCache::Clear();
        ClearHlslStageCompileCache();
    }
}
