#pragma once

namespace EngineConstants
{
    inline constexpr const char* LitVertexShader = "assets/shaders/lit.vert";
    inline constexpr const char* PbrFragmentShader = "assets/shaders/pbr.frag";
    inline constexpr const char* ShadowDepthVertexShader = "assets/shaders/shadow_depth.vert";
    inline constexpr const char* ShadowDepthFragmentShader = "assets/shaders/shadow_depth.frag";
    inline constexpr const char* GridVertexShader = "assets/shaders/grid.vert";
    inline constexpr const char* GridFragmentShader = "assets/shaders/grid.frag";

    inline constexpr const char* IblCubemapVertexShader = "assets/shaders/ibl_cubemap.vert";
    inline constexpr const char* IblEquirectToCubemapFragmentShader = "assets/shaders/ibl_equirect_to_cubemap.frag";
    inline constexpr const char* IblIrradianceFragmentShader = "assets/shaders/ibl_irradiance.frag";
    inline constexpr const char* IblPrefilterFragmentShader = "assets/shaders/ibl_prefilter.frag";
    inline constexpr const char* IblBrdfVertexShader = "assets/shaders/ibl_brdf.vert";
    inline constexpr const char* IblBrdfFragmentShader = "assets/shaders/ibl_brdf.frag";

    inline constexpr const char* EnvironmentHdr = "assets/environment/studio_small_09_1k.hdr";
}
