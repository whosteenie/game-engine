#pragma once

namespace EngineConstants
{
    inline constexpr const char* VelocityDebugFragmentShader = "assets/shaders/post/aa/velocity_debug.ps.hlsl";
    inline constexpr const char* GBufferDebugFragmentShader = "assets/shaders/post/debug/gbuffer_debug.ps.hlsl";
    inline constexpr const char* RadianceAssemblyFragmentShader = "assets/shaders/post/utility/radiance_assembly.ps.hlsl";
    inline constexpr const char* RadianceDebugFragmentShader = "assets/shaders/post/debug/radiance_debug.ps.hlsl";
    inline constexpr const char* TemporalReprojectFragmentShader = "assets/shaders/post/aa/temporal_reproject.ps.hlsl";
    inline constexpr const char* GiDepthHistoryFragmentShader = "assets/shaders/post/utility/gi_depth_history.ps.hlsl";
    inline constexpr const char* DlssMotionDilateFragmentShader =
        "assets/shaders/post/utility/dlss_motion_dilate.ps.hlsl";
    inline constexpr const char* DlssMotionCopyFragmentShader =
        "assets/shaders/post/utility/dlss_motion_copy.ps.hlsl";
    inline constexpr const char* DlssZeroMotionFragmentShader =
        "assets/shaders/post/utility/dlss_zero_motion.ps.hlsl";
    inline constexpr const char* GiTemporalDebugFragmentShader = "assets/shaders/post/utility/gi_temporal_debug.ps.hlsl";
    inline constexpr const char* SsgiNoiseInjectFragmentShader = "assets/shaders/post/screen_space/ssgi_noise_inject.ps.hlsl";
    inline constexpr const char* SsgiDenoiseSpatialFragmentShader = "assets/shaders/post/screen_space/ssgi_denoise_spatial.ps.hlsl";
    inline constexpr const char* SsgiDenoiseDebugFragmentShader = "assets/shaders/post/screen_space/ssgi_denoise_debug.ps.hlsl";
    inline constexpr const char* SsgiTraceFragmentShader = "assets/shaders/post/screen_space/ssgi_trace.ps.hlsl";
    inline constexpr const char* SsrSceneColorFragmentShader = "assets/shaders/post/screen_space/ssr_scene_color.ps.hlsl";
    inline constexpr const char* SsrDebugFragmentShader = "assets/shaders/post/screen_space/ssr_debug.ps.hlsl";
    inline constexpr const char* SsrTraceFragmentShader = "assets/shaders/post/screen_space/ssr_trace.ps.hlsl";
    inline constexpr const char* SsrTraceDebugFragmentShader = "assets/shaders/post/screen_space/ssr_trace_debug.ps.hlsl";
    inline constexpr const char* SsrDenoiseDebugFragmentShader = "assets/shaders/post/screen_space/ssr_denoise_debug.ps.hlsl";
    inline constexpr const char* SsrSvgfTemporalFragmentShader = "assets/shaders/post/screen_space/ssr_svgf_temporal.ps.hlsl";
    inline constexpr const char* SsrSvgfVarianceTemporalFragmentShader =
        "assets/shaders/post/screen_space/ssr_svgf_variance_temporal.ps.hlsl";
    inline constexpr const char* SsrSvgfAtrousFragmentShader = "assets/shaders/post/screen_space/ssr_svgf_atrous.ps.hlsl";
    inline constexpr const char* SsrUpscaleFragmentShader = "assets/shaders/post/screen_space/ssr_upscale.ps.hlsl";
    inline constexpr const char* SsrIndirectFragmentShader = "assets/shaders/post/screen_space/ssr_indirect.ps.hlsl";
    inline constexpr const char* LitVertexShader = "assets/shaders/geometry/lit.vs.hlsl";
    inline constexpr const char* PbrFragmentShader = "assets/shaders/geometry/pbr.ps.hlsl";
    inline constexpr const char* SceneGBufferAmplificationShader =
        "assets/shaders/geometry/scene_gbuffer.as.hlsl";
    inline constexpr const char* SceneGBufferMeshShader = "assets/shaders/geometry/scene_gbuffer.ms.hlsl";
    inline constexpr const char* SceneGBufferMeshFragmentShader =
        "assets/shaders/geometry/scene_gbuffer_mesh.ps.hlsl";
    inline constexpr const char* SceneShadowAmplificationShader =
        "assets/shaders/shadows/scene_shadow.as.hlsl";
    inline constexpr const char* SceneShadowMeshShader = "assets/shaders/shadows/scene_shadow.ms.hlsl";
    inline constexpr const char* ShadowDepthVertexShader = "assets/shaders/shadows/shadow_depth.vs.hlsl";
    inline constexpr const char* ShadowDepthFragmentShader = "assets/shaders/shadows/shadow_depth.ps.hlsl";
    inline constexpr const char* GridVertexShader = "assets/shaders/editor/grid.vs.hlsl";
    inline constexpr const char* GridFragmentShader = "assets/shaders/editor/grid.ps.hlsl";
    inline constexpr const char* GizmoLineVertexShader = "assets/shaders/editor/gizmo_line.vs.hlsl";
    inline constexpr const char* LineFragmentShader = "assets/shaders/editor/line.ps.hlsl";
    inline constexpr const char* SelectionOutlineVertexShader = "assets/shaders/editor/selection_outline.vs.hlsl";
    inline constexpr const char* SelectionOutlineFragmentShader = "assets/shaders/editor/selection_outline.ps.hlsl";
    inline constexpr const char* SelectionMaskVertexShader = "assets/shaders/editor/selection_mask.vs.hlsl";
    inline constexpr const char* SelectionMaskFragmentShader = "assets/shaders/editor/selection_mask.ps.hlsl";
    inline constexpr const char* SelectionEdgeFragmentShader = "assets/shaders/editor/selection_edge.ps.hlsl";
    inline constexpr const char* SelectionGlowFragmentShader = "assets/shaders/editor/selection_glow.ps.hlsl";
    inline constexpr const char* SelectionSharpFragmentShader = "assets/shaders/editor/selection_sharp.ps.hlsl";

    inline constexpr const char* FullscreenVertexShader = "assets/shaders/post/utility/fullscreen.vs.hlsl";
    inline constexpr const char* SsaoFragmentShader = "assets/shaders/post/screen_space/ssao.ps.hlsl";
    inline constexpr const char* GtaoFragmentShader = "assets/shaders/post/screen_space/gtao.ps.hlsl";
    inline constexpr const char* SsaoBlurFragmentShader = "assets/shaders/post/screen_space/ssao_blur.ps.hlsl";
    inline constexpr const char* ScreenCompositeFragmentShader = "assets/shaders/post/utility/screen_composite.ps.hlsl";
    inline constexpr const char* BloomExtractFragmentShader = "assets/shaders/post/bloom/bloom_extract.ps.hlsl";
    inline constexpr const char* BloomBlurFragmentShader = "assets/shaders/post/bloom/bloom_blur.ps.hlsl";
    inline constexpr const char* BloomTemporalFragmentShader = "assets/shaders/post/bloom/bloom_temporal.ps.hlsl";
    inline constexpr const char* ShadowBlurFragmentShader = "assets/shaders/shadows/shadow_blur.ps.hlsl";
    inline constexpr const char* TonemapFragmentShader = "assets/shaders/post/bloom/tonemap.ps.hlsl";
    inline constexpr const char* FxaaFragmentShader = "assets/shaders/post/aa/fxaa.ps.hlsl";
    inline constexpr const char* DownsampleFragmentShader = "assets/shaders/post/utility/downsample.ps.hlsl";
    inline constexpr const char* PtOpticalLayersFragmentShader = "assets/shaders/post/utility/pt_optical_layers.ps.hlsl";
    inline constexpr const char* TaaFragmentShader = "assets/shaders/post/aa/taa.ps.hlsl";
    inline constexpr const char* SmaaEdgeFragmentShader = "assets/shaders/post/aa/smaa_edge.ps.hlsl";
    inline constexpr const char* SmaaNeighborFragmentShader = "assets/shaders/post/aa/smaa_neighbor.ps.hlsl";
    inline constexpr const char* MipmapGenFragmentShader = "assets/shaders/post/utility/mipmap_gen.ps.hlsl";
    inline constexpr const char* DebugChannelFragmentShader = "assets/shaders/post/debug/debug_channel.ps.hlsl";
    inline constexpr const char* RtReflectionResolveFragmentShader =
        "assets/shaders/raytracing/post/rt_reflection_resolve.ps.hlsl";
    inline constexpr const char* DepthBlitFragmentShader = "assets/shaders/post/utility/depth_blt.ps.hlsl";
    inline constexpr const char* PtSkyMotionPatchFragmentShader =
        "assets/shaders/raytracing/post/pt_sky_motion_patch.ps.hlsl";
    inline constexpr const char* MsaaDepthResolveFragmentShader =
        "assets/shaders/post/utility/msaa_depth_resolve.ps.hlsl";

    inline constexpr const char* IblCubemapVertexShader = "assets/shaders/environment/ibl_cubemap.vs.hlsl";
    inline constexpr const char* IblEquirectToCubemapFragmentShader =
        "assets/shaders/environment/ibl_equirect_to_cubemap.ps.hlsl";
    inline constexpr const char* IblIrradianceFragmentShader = "assets/shaders/environment/ibl_irradiance.ps.hlsl";
    inline constexpr const char* IblPrefilterFragmentShader = "assets/shaders/environment/ibl_prefilter.ps.hlsl";
    inline constexpr const char* IblBrdfVertexShader = "assets/shaders/environment/ibl_brdf.vs.hlsl";
    inline constexpr const char* IblBrdfFragmentShader = "assets/shaders/environment/ibl_brdf.ps.hlsl";

    inline constexpr const char* SkyboxVertexShader = "assets/shaders/environment/skybox.vs.hlsl";
    inline constexpr const char* SkyboxFragmentShader = "assets/shaders/environment/skybox.ps.hlsl";
    inline constexpr const char* SkyBackgroundFragmentShader = "assets/shaders/environment/sky_background.ps.hlsl";

    inline constexpr const char* DxrSmokeLibraryShader = "assets/shaders/raytracing/libraries/dxr_smoke.hlsl";
    inline constexpr const char* DxrPrimaryDebugLibraryShader = "assets/shaders/raytracing/libraries/primary_debug.hlsl";
    inline constexpr const char* DxrPrimaryDebugFragmentShader = "assets/shaders/raytracing/post/dxr_primary_debug.ps.hlsl";
    inline constexpr const char* PtAccumulateFragmentShader = "assets/shaders/raytracing/post/pt_accumulate.ps.hlsl";
    inline constexpr const char* PtMeanFragmentShader = "assets/shaders/raytracing/post/pt_mean.ps.hlsl";
    inline constexpr const char* PtTemporalStatsFragmentShader =
        "assets/shaders/raytracing/post/pt_temporal_stats.ps.hlsl";
    inline constexpr const char* PtTemporalQualityFragmentShader =
        "assets/shaders/raytracing/post/pt_temporal_quality.ps.hlsl";
    inline constexpr const char* PtTemporalStatsDebugFragmentShader =
        "assets/shaders/raytracing/post/pt_temporal_stats_debug.ps.hlsl";
    inline constexpr const char* PtMotionReprojectionDebugFragmentShader =
        "assets/shaders/raytracing/post/pt_motion_reprojection_debug.ps.hlsl";
    inline constexpr const char* PtMotionDepthCopyFragmentShader =
        "assets/shaders/raytracing/post/pt_motion_depth_copy.ps.hlsl";
    inline constexpr const char* PtBoilMetricFragmentShader =
        "assets/shaders/raytracing/post/pt_boil_metric.ps.hlsl";
    inline constexpr const char* DxrReflectionsLibraryShader = "assets/shaders/raytracing/libraries/reflections.hlsl";
    inline constexpr const char* DxrIndirectFragmentShader = "assets/shaders/raytracing/post/dxr_indirect.ps.hlsl";
    inline constexpr const char* DxrShadowsLibraryShader = "assets/shaders/raytracing/libraries/shadows.hlsl";
    inline constexpr const char* DxrShadowDebugFragmentShader = "assets/shaders/raytracing/post/dxr_shadow_debug.ps.hlsl";
    inline constexpr const char* DxrPathTracerLibraryShader = "assets/shaders/raytracing/path_tracing/path_tracer.hlsl";
    inline constexpr const char* DxrGiLibraryShader = "assets/shaders/raytracing/libraries/diffuse_gi.hlsl";
    inline constexpr const char* DxrRestirLibraryShader = "assets/shaders/raytracing/path_tracing/restir_di_temporal.hlsl";
    inline constexpr const char* DxrGiInjectFragmentShader = "assets/shaders/raytracing/post/dxr_gi_inject.ps.hlsl";
    inline constexpr const char* RrGuidesFragmentShader = "assets/shaders/raytracing/post/rr_guides.ps.hlsl";

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
