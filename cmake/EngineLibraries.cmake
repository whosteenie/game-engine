# engine-render + engine-dxr static libraries (HK-E6).
# Linked by game-engine and d3d12-render-tests (HK-E7).

set(ENGINE_RENDER_SOURCES
    src/engine/camera/Camera.cpp

    src/engine/platform/ui/ImGuiLayer.cpp
    src/engine/platform/ui/ImGuiFonts.cpp
    src/engine/platform/system/BackgroundWork.cpp
    src/engine/platform/tooling/NativeProgressWindow.cpp
    src/engine/platform/tooling/ProjectLoadBenchmark.cpp
    src/engine/platform/input/Input.cpp
    src/engine/platform/input/InputDiagnostics.cpp
    src/engine/platform/diagnostics/EngineLog.cpp
    src/engine/platform/diagnostics/EngineDiagnostics.cpp
    src/engine/platform/system/CrashHandler.cpp
    src/engine/platform/system/ExceptionMessage.cpp
    src/engine/platform/diagnostics/FrameDiagnostics.cpp
    src/engine/platform/diagnostics/ProjectLoadTrace.cpp
    src/engine/platform/diagnostics/SceneRenderTrace.cpp
    src/engine/platform/diagnostics/RenderPathDiagnostics.cpp
    src/engine/platform/system/SystemResources.cpp

    src/engine/rhi/GfxContext.cpp
    src/engine/rhi/GpuProfiler.cpp
    src/engine/rhi/DlssContext.cpp
    src/engine/rhi/HresultFormat.cpp
    src/engine/rhi/d3d12/FixedDescriptorHeap.cpp
    src/engine/rhi/d3d12/GpuBuffer.cpp
    src/engine/rhi/d3d12/HlslCompiler.cpp

    src/engine/lighting/Light.cpp
    src/engine/components/LightComponent.cpp
    src/engine/components/CameraComponent.cpp
    src/engine/components/ColliderComponent.cpp
    src/engine/components/RigidBodyComponent.cpp
    src/engine/lighting/SceneLighting.cpp
    src/engine/scene/InspectorComponentOrder.cpp
    src/engine/scene/inspector/ComponentOrderJson.cpp
    src/engine/scene/RotationUtils.cpp
    src/engine/scene/SceneObject.cpp
    src/engine/scene/SceneObjectComponents.cpp
    src/engine/scene/Transform.cpp
    src/engine/scene/ScenePicker.cpp
    src/engine/scene/SceneHierarchy.cpp
    src/engine/scene/ScenePrimitive.cpp

    src/engine/gizmos/GizmoGeometry.cpp
    src/engine/gizmos/GizmoDraw.cpp
    src/engine/gizmos/CameraGizmoRenderer.cpp
    src/engine/gizmos/LightGizmoRenderer.cpp
    src/engine/gizmos/ColliderGizmoRenderer.cpp
    src/engine/gizmos/SelectionRenderer.cpp

    src/engine/lighting/ShadowMapMath.cpp
    src/engine/lighting/LightingProbe.cpp
    src/engine/lighting/CascadedShadowMap.cpp
    src/engine/rendering/core/RenderDebug.cpp
    src/engine/lighting/IBL.cpp
    src/engine/lighting/EnvironmentMap.cpp
    src/engine/lighting/IrradianceSh.cpp
    src/engine/lighting/environment/Importance.cpp

    src/engine/rendering/resources/Material.cpp
    src/engine/rendering/resources/Texture.cpp
    src/engine/rendering/scene/GpuScene.cpp
    src/engine/assets/TextureCache.cpp
    src/engine/rendering/resources/MaterialTextures.cpp
    src/engine/rendering/resources/Mesh.cpp
    src/engine/rendering/passes/MeshShaderGBufferRenderer.cpp
    src/engine/rendering/passes/MeshShaderShadowRenderer.cpp
    src/engine/rendering/passes/GridRenderer.cpp
    src/engine/rendering/passes/SkyboxRenderer.cpp
      src/engine/assets/gltf/Images.cpp
      src/engine/assets/gltf/Importer.cpp
      src/engine/assets/gltf/Materials.cpp
      src/engine/assets/gltf/Meshes.cpp
      src/engine/assets/gltf/Nodes.cpp
    src/engine/assets/TangentSpace.cpp
    src/engine/assets/StbImage.cpp
    src/engine/assets/ProjectAssets.cpp
    src/engine/assets/FileDialog.cpp
    src/engine/rendering/core/Renderer.cpp
    src/engine/rendering/resources/Framebuffer.cpp
    src/engine/rendering/core/HistoryCompatibility.cpp
    src/engine/rendering/core/ReconstructionJitter.cpp
      src/engine/rendering/post/ScreenSpaceEffects.cpp
      src/engine/rendering/post/effects/Controls.cpp
      src/engine/rendering/post/effects/RayTracing.cpp
      src/engine/rendering/post/effects/Temporal.cpp
      src/engine/rendering/post/effects/Apply.cpp
    src/engine/rendering/post/effects/Settings.cpp
    src/engine/rendering/post/PostProcessDraw.cpp
    src/engine/rendering/post/DxrDebugBlitPass.cpp
    src/engine/rendering/post/AmbientOcclusionPass.cpp
    src/engine/rendering/post/BloomTonemapPass.cpp
    src/engine/rendering/post/ScreenSpaceReflectionPass.cpp
    src/engine/rendering/post/ScreenSpaceGiPass.cpp
    src/engine/rendering/post/AntiAliasingPass.cpp
    src/engine/rendering/post/DlssResolvePass.cpp
    src/engine/rendering/post/PathTracerDisplayPass.cpp
    src/engine/rendering/post/ScreenCompositePass.cpp
    src/engine/rendering/post/PostProcessDebugPass.cpp
    src/engine/rendering/core/DxrSettings.cpp
    src/engine/rendering/core/PtTemporalHistory.cpp
    src/engine/lighting/DirectionalShadowSettings.cpp
    src/engine/rendering/shaders/Shader.cpp
    src/engine/rendering/shaders/ShaderCache.cpp

    src/primitives/Cube.cpp
    src/primitives/PrimitiveMeshUtils.cpp
    src/primitives/Sphere.cpp
    src/primitives/Cylinder.cpp
    src/primitives/Capsule.cpp
    src/primitives/Plane.cpp
)

set(ENGINE_DXR_SOURCES
    src/engine/raytracing/core/DxrContext.cpp
    src/engine/raytracing/core/DxrGpuResource.cpp
    src/engine/raytracing/acceleration/Blas.cpp
    src/engine/raytracing/acceleration/BlasCache.cpp
    src/engine/raytracing/acceleration/Tlas.cpp
    src/engine/raytracing/acceleration/DxrAccelerationStructures.cpp
    src/engine/raytracing/pipeline/DxrShaderCache.cpp
    src/engine/raytracing/pipeline/DxrRootSignature.cpp
    src/engine/raytracing/pipeline/DxrPipeline.cpp
    src/engine/raytracing/pipeline/ShaderBindingTable.cpp
    src/engine/raytracing/dispatch/DxrDispatchContext.cpp
    src/engine/raytracing/dispatch/DxrDispatchBase.cpp
    src/engine/raytracing/dispatch/DxrSmokeDispatch.cpp
    src/engine/raytracing/dispatch/DxrPrimaryDebugDispatch.cpp
    src/engine/raytracing/dispatch/DxrPathTracerDispatch.cpp
    src/engine/raytracing/dispatch/DxrReflectionsDispatch.cpp
    src/engine/raytracing/dispatch/DxrShadowsDispatch.cpp
    src/engine/raytracing/dispatch/DxrGiDispatch.cpp
    src/engine/raytracing/dispatch/DxrRestirDispatch.cpp
    src/engine/raytracing/denoising/NrdCommon.cpp
    src/engine/raytracing/denoising/NrdDenoiser.cpp
    src/engine/raytracing/denoising/NrdShadowDenoiser.cpp
)

add_library(engine-render STATIC ${ENGINE_RENDER_SOURCES})
add_library(engine-dxr STATIC ${ENGINE_DXR_SOURCES})
game_engine_enable_msvc_parallel_compile(engine-render)
game_engine_enable_msvc_parallel_compile(engine-dxr)

function(game_engine_apply_render_library_config target)
    target_include_directories(${target} PUBLIC
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/vendor/stb"
        "${CMAKE_SOURCE_DIR}/vendor/IconFontCppHeaders"
        "${tinygltf_SOURCE_DIR}"
        "${json_SOURCE_DIR}/include")
    target_compile_definitions(${target} PUBLIC NOMINMAX)
    target_link_libraries(${target} PUBLIC
        glfw
        glm::glm
        imgui_lib
        imguizmo_lib
        mikktspace
        nlohmann_json::nlohmann_json
        D3D12MemoryAllocator
        d3d12
        dxgi
        dxguid
        dxcompiler)
    if(GAME_ENGINE_HAS_DLSS)
        target_include_directories(${target} PUBLIC "${STREAMLINE_INCLUDE_DIR}")
        target_compile_definitions(${target} PUBLIC GAME_ENGINE_ENABLE_DLSS)
    endif()
    if(GAME_ENGINE_D3D12_DEBUG_LAYER)
        target_compile_definitions(${target} PUBLIC GAME_ENGINE_D3D12_DEBUG_LAYER)
    endif()
    if(WIN32)
        target_link_libraries(${target} PUBLIC Dbghelp comdlg32 shell32 ole32 comctl32)
    endif()
endfunction()

game_engine_apply_render_library_config(engine-render)

# S0-P1 runtime snapshot: the device-owning static library needs the same pinned Agility identity
# as the executable export.  This is diagnostics metadata only; it does not alter loader setup.
if(WIN32 AND GAME_ENGINE_USE_AGILITY_SDK)
    target_include_directories(engine-render PUBLIC
        "${GAME_ENGINE_AGILITY_SDK_ROOT}/build/native/include")
    target_compile_definitions(engine-render PUBLIC
        GAME_ENGINE_AGILITY_SDK_VERSION=${GAME_ENGINE_AGILITY_SDK_LOADER_VERSION}
        GAME_ENGINE_AGILITY_SDK_PACKAGE_VERSION="${GAME_ENGINE_AGILITY_SDK_PACKAGE_VERSION}")
endif()

target_link_libraries(engine-dxr PUBLIC engine-render NRD)
game_engine_apply_render_library_config(engine-dxr)

if(MSVC)
    target_link_options(engine-render PRIVATE /NODEFAULTLIB:libcmt)
    target_link_options(engine-dxr PRIVATE /NODEFAULTLIB:libcmt)
endif()
