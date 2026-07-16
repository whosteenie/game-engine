# engine-render + engine-dxr static libraries (HK-E6).
# Linked by game-engine and d3d12-render-tests (HK-E7).

set(ENGINE_RENDER_SOURCES
    src/engine/camera/Camera.cpp

    src/engine/platform/ImGuiLayer.cpp
    src/engine/platform/ImGuiFonts.cpp
    src/engine/platform/NativeProgressWindow.cpp
    src/engine/platform/ProjectLoadBenchmark.cpp
    src/engine/platform/Input.cpp
    src/engine/platform/InputDiagnostics.cpp
    src/engine/platform/EngineLog.cpp
    src/engine/platform/EngineDiagnostics.cpp
    src/engine/platform/CrashHandler.cpp
    src/engine/platform/ExceptionMessage.cpp
    src/engine/platform/FrameDiagnostics.cpp
    src/engine/platform/ProjectLoadTrace.cpp
    src/engine/platform/SceneRenderTrace.cpp
    src/engine/platform/RenderPathDiagnostics.cpp
    src/engine/platform/SystemResources.cpp

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
    src/engine/scene/InspectorComponentOrderJson.cpp
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
    src/engine/rendering/RenderDebug.cpp
    src/engine/lighting/IBL.cpp
    src/engine/lighting/EnvironmentMap.cpp
    src/engine/lighting/IrradianceSh.cpp
    src/engine/lighting/EnvironmentImportanceSampling.cpp

    src/engine/rendering/Material.cpp
    src/engine/rendering/Texture.cpp
    src/engine/assets/TextureCache.cpp
    src/engine/rendering/MaterialTextures.cpp
    src/engine/rendering/Mesh.cpp
    src/engine/rendering/MeshShaderGBufferRenderer.cpp
    src/engine/rendering/MeshShaderShadowRenderer.cpp
    src/engine/rendering/GridRenderer.cpp
    src/engine/rendering/SkyboxRenderer.cpp
    src/engine/assets/ModelImporter.cpp
    src/engine/assets/TangentSpace.cpp
    src/engine/assets/StbImage.cpp
    src/engine/assets/ProjectAssets.cpp
    src/engine/assets/FileDialog.cpp
    src/engine/rendering/Renderer.cpp
    src/engine/rendering/Framebuffer.cpp
    src/engine/rendering/HistoryCompatibility.cpp
    src/engine/rendering/ScreenSpaceEffects.cpp
    src/engine/rendering/ScreenSpaceEffectsApply.cpp
    src/engine/rendering/ScreenSpaceEffectsSettings.cpp
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
    src/engine/rendering/DxrSettings.cpp
    src/engine/rendering/PtTemporalHistory.cpp
    src/engine/lighting/DirectionalShadowSettings.cpp
    src/engine/rendering/Shader.cpp
    src/engine/rendering/ShaderCache.cpp

    src/primitives/Cube.cpp
    src/primitives/PrimitiveMeshUtils.cpp
    src/primitives/Sphere.cpp
    src/primitives/Cylinder.cpp
    src/primitives/Capsule.cpp
    src/primitives/Plane.cpp
)

set(ENGINE_DXR_SOURCES
    src/engine/raytracing/DxrContext.cpp
    src/engine/raytracing/DxrGpuResource.cpp
    src/engine/raytracing/Blas.cpp
    src/engine/raytracing/BlasCache.cpp
    src/engine/raytracing/Tlas.cpp
    src/engine/raytracing/DxrAccelerationStructures.cpp
    src/engine/raytracing/DxrShaderCache.cpp
    src/engine/raytracing/DxrRootSignature.cpp
    src/engine/raytracing/DxrPipeline.cpp
    src/engine/raytracing/ShaderBindingTable.cpp
    src/engine/raytracing/DxrDispatchContext.cpp
    src/engine/raytracing/DxrDispatchBase.cpp
    src/engine/raytracing/DxrSmokeDispatch.cpp
    src/engine/raytracing/DxrPrimaryDebugDispatch.cpp
    src/engine/raytracing/DxrPathTracerDispatch.cpp
    src/engine/raytracing/DxrReflectionsDispatch.cpp
    src/engine/raytracing/DxrShadowsDispatch.cpp
    src/engine/raytracing/DxrGiDispatch.cpp
    src/engine/raytracing/DxrRestirDispatch.cpp
    src/engine/raytracing/NrdCommon.cpp
    src/engine/raytracing/NrdDenoiser.cpp
    src/engine/raytracing/NrdShadowDenoiser.cpp
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
