#pragma once

#include "engine/rendering/post/PostProcessDraw.h"

// Per-frame hooks passed into extracted post-process passes (HK-C0).
struct PostProcessContext
{
    PostProcessDraw& draw;
    int renderWidth = 0;
    int renderHeight = 0;
};
