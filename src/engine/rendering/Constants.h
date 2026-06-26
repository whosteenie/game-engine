#pragma once

namespace EngineConstants
{
    inline constexpr const char* VelocityDebugFragmentShader = "assets/shaders/velocity_debug.ps.hlsl";
    inline constexpr const char* GBufferDebugFragmentShader = "assets/shaders/gbuffer_debug.ps.hlsl";
    inline constexpr const char* RadianceAssemblyFragmentShader = "assets/shaders/radiance_assembly.ps.hlsl";
    inline constexpr const char* RadianceDebugFragmentShader = "assets/shaders/radiance_debug.ps.hlsl";
    inline constexpr const char* LitVertexShader = "assets/shaders/lit.vs.hlsl";
    inline constexpr const char* PbrFragmentShader = "assets/shaders/pbr.ps.hlsl";
    inline constexpr const char* ShadowDepthVertexShader = "assets/shaders/shadow_depth.vs.hlsl";
    inline constexpr const char* ShadowDepthFragmentShader = "assets/shaders/shadow_depth.ps.hlsl";
    inline constexpr const char* GridVertexShader = "assets/shaders/grid.vs.hlsl";
    inline constexpr const char* GridFragmentShader = "assets/shaders/grid.ps.hlsl";
    inline constexpr const char* GizmoLineVertexShader = "assets/shaders/gizmo_line.vs.hlsl";
    inline constexpr const char* LineFragmentShader = "assets/shaders/line.ps.hlsl";
    inline constexpr const char* SelectionOutlineVertexShader = "assets/shaders/selection_outline.vs.hlsl";
    inline constexpr const char* SelectionOutlineFragmentShader = "assets/shaders/selection_outline.ps.hlsl";
    inline constexpr const char* SelectionMaskVertexShader = "assets/shaders/selection_mask.vs.hlsl";
    inline constexpr const char* SelectionMaskFragmentShader = "assets/shaders/selection_mask.ps.hlsl";
    inline constexpr const char* SelectionEdgeFragmentShader = "assets/shaders/selection_edge.ps.hlsl";
    inline constexpr const char* SelectionGlowFragmentShader = "assets/shaders/selection_glow.ps.hlsl";
    inline constexpr const char* SelectionSharpFragmentShader = "assets/shaders/selection_sharp.ps.hlsl";

    inline constexpr const char* FullscreenVertexShader = "assets/shaders/fullscreen.vs.hlsl";
    inline constexpr const char* SsaoFragmentShader = "assets/shaders/ssao.ps.hlsl";
    inline constexpr const char* SsaoBlurFragmentShader = "assets/shaders/ssao_blur.ps.hlsl";
    inline constexpr const char* ScreenCompositeFragmentShader = "assets/shaders/screen_composite.ps.hlsl";
    inline constexpr const char* BloomExtractFragmentShader = "assets/shaders/bloom_extract.ps.hlsl";
    inline constexpr const char* BloomBlurFragmentShader = "assets/shaders/bloom_blur.ps.hlsl";
    inline constexpr const char* ShadowBlurFragmentShader = "assets/shaders/shadow_blur.ps.hlsl";
    inline constexpr const char* TonemapFragmentShader = "assets/shaders/tonemap.ps.hlsl";
    inline constexpr const char* FxaaFragmentShader = "assets/shaders/fxaa.ps.hlsl";
    inline constexpr const char* DownsampleFragmentShader = "assets/shaders/downsample.ps.hlsl";
    inline constexpr const char* TaaFragmentShader = "assets/shaders/taa.ps.hlsl";
    inline constexpr const char* SmaaEdgeFragmentShader = "assets/shaders/smaa_edge.ps.hlsl";
    inline constexpr const char* SmaaNeighborFragmentShader = "assets/shaders/smaa_neighbor.ps.hlsl";
    inline constexpr const char* MipmapGenFragmentShader = "assets/shaders/mipmap_gen.ps.hlsl";
    inline constexpr const char* GridCompositeFragmentShader = "assets/shaders/grid_composite.ps.hlsl";
    inline constexpr const char* DebugChannelFragmentShader = "assets/shaders/debug_channel.ps.hlsl";

    inline constexpr const char* IblCubemapVertexShader = "assets/shaders/ibl_cubemap.vs.hlsl";
    inline constexpr const char* IblEquirectToCubemapFragmentShader =
        "assets/shaders/ibl_equirect_to_cubemap.ps.hlsl";
    inline constexpr const char* IblIrradianceFragmentShader = "assets/shaders/ibl_irradiance.ps.hlsl";
    inline constexpr const char* IblPrefilterFragmentShader = "assets/shaders/ibl_prefilter.ps.hlsl";
    inline constexpr const char* IblBrdfVertexShader = "assets/shaders/ibl_brdf.vs.hlsl";
    inline constexpr const char* IblBrdfFragmentShader = "assets/shaders/ibl_brdf.ps.hlsl";

    inline constexpr const char* SkyboxVertexShader = "assets/shaders/skybox.vs.hlsl";
    inline constexpr const char* SkyboxFragmentShader = "assets/shaders/skybox.ps.hlsl";
    inline constexpr const char* SkyBackgroundFragmentShader = "assets/shaders/sky_background.ps.hlsl";

    inline constexpr float BackgroundSrgb[3] = {0.08f, 0.09f, 0.15f};

    inline constexpr const char* EnvironmentHdr = "assets/environment/qwantani_puresky_2k.hdr";
    inline constexpr const char* EnvironmentHdrStudio = "assets/environment/studio_small_09_2k.hdr";

    inline constexpr const char* CubeAlbedoTexture = "assets/textures/cube_albedo.jpg";
    inline constexpr const char* CubeNormalTexture = "assets/textures/cube_normal.jpg";
    inline constexpr const char* CubeAoTexture = "assets/textures/cube_ao.jpg";
    inline constexpr const char* CubeRoughnessTexture = "assets/textures/cube_roughness.jpg";

    inline constexpr const char* FloorAlbedoTexture = "assets/textures/floor_albedo.jpg";
    inline constexpr const char* FloorNormalTexture = "assets/textures/floor_normal.jpg";
    inline constexpr const char* FloorAoTexture = "assets/textures/floor_ao.jpg";
    inline constexpr const char* FloorRoughnessTexture = "assets/textures/floor_roughness.jpg";
}
