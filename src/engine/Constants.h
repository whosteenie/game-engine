#pragma once

namespace EngineConstants
{
    inline constexpr const char* LitVertexShader = "assets/shaders/lit.vert";
    inline constexpr const char* PbrFragmentShader = "assets/shaders/pbr.frag";
    inline constexpr const char* ShadowDepthVertexShader = "assets/shaders/shadow_depth.vert";
    inline constexpr const char* ShadowDepthFragmentShader = "assets/shaders/shadow_depth.frag";
    inline constexpr const char* GridVertexShader = "assets/shaders/grid.vert";
    inline constexpr const char* GridFragmentShader = "assets/shaders/grid.frag";
    inline constexpr const char* LineFragmentShader = "assets/shaders/line.frag";
    inline constexpr const char* SelectionOutlineVertexShader = "assets/shaders/selection_outline.vert";
    inline constexpr const char* SelectionOutlineFragmentShader = "assets/shaders/selection_outline.frag";
    inline constexpr const char* SelectionMaskVertexShader = "assets/shaders/selection_mask.vert";
    inline constexpr const char* SelectionMaskFragmentShader = "assets/shaders/selection_mask.frag";
    inline constexpr const char* SelectionEdgeFragmentShader = "assets/shaders/selection_edge.frag";

    inline constexpr const char* FullscreenVertexShader = "assets/shaders/fullscreen.vert";
    inline constexpr const char* SsaoFragmentShader = "assets/shaders/ssao.frag";
    inline constexpr const char* SsaoBlurFragmentShader = "assets/shaders/ssao_blur.frag";
    inline constexpr const char* ContactShadowFragmentShader = "assets/shaders/contact_shadow.frag";
    inline constexpr const char* ScreenCompositeFragmentShader = "assets/shaders/screen_composite.frag";
    inline constexpr const char* BloomExtractFragmentShader = "assets/shaders/bloom_extract.frag";
    inline constexpr const char* BloomBlurFragmentShader = "assets/shaders/bloom_blur.frag";
    inline constexpr const char* TonemapFragmentShader = "assets/shaders/tonemap.frag";

    inline constexpr const char* IblCubemapVertexShader = "assets/shaders/ibl_cubemap.vert";
    inline constexpr const char* IblEquirectToCubemapFragmentShader = "assets/shaders/ibl_equirect_to_cubemap.frag";
    inline constexpr const char* IblIrradianceFragmentShader = "assets/shaders/ibl_irradiance.frag";
    inline constexpr const char* IblPrefilterFragmentShader = "assets/shaders/ibl_prefilter.frag";
    inline constexpr const char* IblBrdfVertexShader = "assets/shaders/ibl_brdf.vert";
    inline constexpr const char* IblBrdfFragmentShader = "assets/shaders/ibl_brdf.frag";

    inline constexpr const char* EnvironmentHdr = "assets/environment/studio_small_09_1k.hdr";

    inline constexpr const char* CubeAlbedoTexture = "assets/textures/cube_albedo.jpg";
    inline constexpr const char* CubeNormalTexture = "assets/textures/cube_normal.jpg";
    inline constexpr const char* CubeAoTexture = "assets/textures/cube_ao.jpg";
    inline constexpr const char* CubeRoughnessTexture = "assets/textures/cube_roughness.jpg";

    inline constexpr const char* FloorAlbedoTexture = "assets/textures/floor_albedo.jpg";
    inline constexpr const char* FloorNormalTexture = "assets/textures/floor_normal.jpg";
    inline constexpr const char* FloorAoTexture = "assets/textures/floor_ao.jpg";
    inline constexpr const char* FloorRoughnessTexture = "assets/textures/floor_roughness.jpg";
}
