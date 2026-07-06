#pragma once

namespace EngineConstants
{
    inline constexpr const char* VelocityDebugFragmentShader = "assets/shaders/velocity_debug.ps.hlsl";
    inline constexpr const char* GBufferDebugFragmentShader = "assets/shaders/gbuffer_debug.ps.hlsl";
    inline constexpr const char* RadianceAssemblyFragmentShader = "assets/shaders/radiance_assembly.ps.hlsl";
    inline constexpr const char* RadianceDebugFragmentShader = "assets/shaders/radiance_debug.ps.hlsl";
    inline constexpr const char* TemporalReprojectFragmentShader = "assets/shaders/temporal_reproject.ps.hlsl";
    inline constexpr const char* GiDepthHistoryFragmentShader = "assets/shaders/gi_depth_history.ps.hlsl";
    inline constexpr const char* GiTemporalDebugFragmentShader = "assets/shaders/gi_temporal_debug.ps.hlsl";
    inline constexpr const char* SsgiNoiseInjectFragmentShader = "assets/shaders/ssgi_noise_inject.ps.hlsl";
    inline constexpr const char* SsgiDenoiseSpatialFragmentShader = "assets/shaders/ssgi_denoise_spatial.ps.hlsl";
    inline constexpr const char* SsgiDenoiseDebugFragmentShader = "assets/shaders/ssgi_denoise_debug.ps.hlsl";
    inline constexpr const char* SsgiTraceFragmentShader = "assets/shaders/ssgi_trace.ps.hlsl";
    inline constexpr const char* SsrSceneColorFragmentShader = "assets/shaders/ssr_scene_color.ps.hlsl";
    inline constexpr const char* SsrDebugFragmentShader = "assets/shaders/ssr_debug.ps.hlsl";
    inline constexpr const char* SsrTraceFragmentShader = "assets/shaders/ssr_trace.ps.hlsl";
    inline constexpr const char* SsrTraceDebugFragmentShader = "assets/shaders/ssr_trace_debug.ps.hlsl";
    inline constexpr const char* SsrDenoiseSpatialFragmentShader = "assets/shaders/ssr_denoise_spatial.ps.hlsl";
    inline constexpr const char* SsrDenoiseDebugFragmentShader = "assets/shaders/ssr_denoise_debug.ps.hlsl";
    inline constexpr const char* SsrTemporalFragmentShader = "assets/shaders/ssr_temporal.ps.hlsl";
    inline constexpr const char* SsrSvgfTemporalFragmentShader = "assets/shaders/ssr_svgf_temporal.ps.hlsl";
    inline constexpr const char* SsrSvgfVarianceTemporalFragmentShader =
        "assets/shaders/ssr_svgf_variance_temporal.ps.hlsl";
    inline constexpr const char* SsrSvgfAtrousFragmentShader = "assets/shaders/ssr_svgf_atrous.ps.hlsl";
    inline constexpr const char* SsrUpscaleFragmentShader = "assets/shaders/ssr_upscale.ps.hlsl";
    inline constexpr const char* SsrIndirectFragmentShader = "assets/shaders/ssr_indirect.ps.hlsl";
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
    inline constexpr const char* GtaoFragmentShader = "assets/shaders/gtao.ps.hlsl";
    inline constexpr const char* SsaoBlurFragmentShader = "assets/shaders/ssao_blur.ps.hlsl";
    inline constexpr const char* ScreenCompositeFragmentShader = "assets/shaders/screen_composite.ps.hlsl";
    inline constexpr const char* BloomExtractFragmentShader = "assets/shaders/bloom_extract.ps.hlsl";
    inline constexpr const char* BloomBlurFragmentShader = "assets/shaders/bloom_blur.ps.hlsl";
    inline constexpr const char* BloomTemporalFragmentShader = "assets/shaders/bloom_temporal.ps.hlsl";
    inline constexpr const char* ShadowBlurFragmentShader = "assets/shaders/shadow_blur.ps.hlsl";
    inline constexpr const char* TonemapFragmentShader = "assets/shaders/tonemap.ps.hlsl";
    inline constexpr const char* FxaaFragmentShader = "assets/shaders/fxaa.ps.hlsl";
    inline constexpr const char* DownsampleFragmentShader = "assets/shaders/downsample.ps.hlsl";
    inline constexpr const char* TaaFragmentShader = "assets/shaders/taa.ps.hlsl";
    inline constexpr const char* SmaaEdgeFragmentShader = "assets/shaders/smaa_edge.ps.hlsl";
    inline constexpr const char* SmaaNeighborFragmentShader = "assets/shaders/smaa_neighbor.ps.hlsl";
    inline constexpr const char* MipmapGenFragmentShader = "assets/shaders/mipmap_gen.ps.hlsl";
    inline constexpr const char* DebugChannelFragmentShader = "assets/shaders/debug_channel.ps.hlsl";
    inline constexpr const char* RtReflectionResolveFragmentShader =
        "assets/shaders/rt_reflection_resolve.ps.hlsl";
    inline constexpr const char* DepthBlitFragmentShader = "assets/shaders/depth_blt.ps.hlsl";
    inline constexpr const char* MsaaDepthResolveFragmentShader =
        "assets/shaders/msaa_depth_resolve.ps.hlsl";

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

    inline constexpr const char* DxrSmokeLibraryShader = "assets/shaders/dxr/dxr_smoke.hlsl";
    inline constexpr const char* DxrPrimaryDebugLibraryShader = "assets/shaders/dxr/primary_debug.hlsl";
    inline constexpr const char* DxrPrimaryDebugFragmentShader = "assets/shaders/dxr_primary_debug.ps.hlsl";
    inline constexpr const char* DxrReflectionsLibraryShader = "assets/shaders/dxr/reflections.hlsl";
    inline constexpr const char* DxrIndirectFragmentShader = "assets/shaders/dxr_indirect.ps.hlsl";

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
