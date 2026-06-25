cmake_minimum_required(VERSION 3.20)

project(game-engine VERSION 0.1.0 LANGUAGES CXX)



set(CMAKE_CXX_STANDARD 17)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    # Allow parallel CL.exe invocations (MSBuild /m) without vc143.pdb lock failures.
    add_compile_options(/FS)
endif()

option(GAME_ENGINE_D3D12_DEBUG_LAYER "Enable the D3D12 debug layer (Debug builds only)" OFF)



include(FetchContent)



FetchContent_Declare(

    glfw

    GIT_REPOSITORY https://github.com/glfw/glfw.git

    GIT_TAG 3.4

)



set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)

set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)

set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)



FetchContent_MakeAvailable(glfw)



FetchContent_Declare(

    glm

    GIT_REPOSITORY https://github.com/g-truc/glm.git

    GIT_TAG 1.0.1

)

FetchContent_MakeAvailable(glm)



FetchContent_Declare(

    imgui

    GIT_REPOSITORY https://github.com/ocornut/imgui.git

    GIT_TAG docking # Docking branch required for editor layout (Phase 1+)

)

FetchContent_MakeAvailable(imgui)



FetchContent_Declare(

    tinygltf

    GIT_REPOSITORY https://github.com/syoyo/tinygltf.git

    GIT_TAG v2.9.3

)

FetchContent_MakeAvailable(tinygltf)



FetchContent_Declare(

    json

    GIT_REPOSITORY https://github.com/nlohmann/json.git

    GIT_TAG v3.11.3

)

FetchContent_MakeAvailable(json)



FetchContent_Declare(

    imguizmo

    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git

    GIT_TAG be8aa4aeab86b402701c8c1df011bd8cd776760b

)



if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

FetchContent_GetProperties(imguizmo)

if(NOT imguizmo_POPULATED)
    FetchContent_Populate(imguizmo)
endif()



set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    JoltPhysics
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG v5.3.0
    SOURCE_SUBDIR Build
)

FetchContent_MakeAvailable(JoltPhysics)

FetchContent_Declare(
    D3D12MemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator.git
    GIT_TAG master
)
FetchContent_GetProperties(D3D12MemoryAllocator)
if(NOT D3D12MemoryAllocator_POPULATED)
    FetchContent_Populate(D3D12MemoryAllocator)
endif()

add_library(D3D12MemoryAllocator STATIC
    ${d3d12memoryallocator_SOURCE_DIR}/src/D3D12MemAlloc.cpp
)
target_include_directories(D3D12MemoryAllocator PUBLIC
    ${d3d12memoryallocator_SOURCE_DIR}/include
    ${d3d12memoryallocator_SOURCE_DIR}/src
)

FetchContent_Declare(
    mikktspace
    GIT_REPOSITORY https://github.com/mmikk/MikkTSpace.git
    GIT_TAG master
)
FetchContent_GetProperties(mikktspace)
if(NOT mikktspace_POPULATED)
    FetchContent_Populate(mikktspace)
endif()

add_library(mikktspace STATIC
    ${mikktspace_SOURCE_DIR}/mikktspace.c
)
target_include_directories(mikktspace PUBLIC ${mikktspace_SOURCE_DIR})

add_library(imgui_lib STATIC

    ${imgui_SOURCE_DIR}/imgui.cpp

    ${imgui_SOURCE_DIR}/imgui_draw.cpp

    ${imgui_SOURCE_DIR}/imgui_tables.cpp

    ${imgui_SOURCE_DIR}/imgui_widgets.cpp

    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp

    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp

)



target_include_directories(imgui_lib PUBLIC

    ${imgui_SOURCE_DIR}

    ${imgui_SOURCE_DIR}/backends)



target_link_libraries(imgui_lib PUBLIC glfw)



add_library(imguizmo_lib STATIC

    ${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp

)



target_include_directories(imguizmo_lib PUBLIC

    ${imguizmo_SOURCE_DIR}/src)



target_compile_definitions(imguizmo_lib PUBLIC IMGUI_DEFINE_MATH_OPERATORS)



target_link_libraries(imguizmo_lib PUBLIC imgui_lib glm::glm)



add_executable(game-engine

    src/main.cpp

    src/app/core/Application.cpp

    src/app/editor/EditorSettings.cpp


    src/app/editor/EditorDockSpace.cpp

    src/app/editor/EditorDockLayout.cpp

    src/app/editor/MainMenuBar.cpp
    src/app/editor/EditorTopToolbar.cpp

    src/app/editor/EditorClipboard.cpp

    src/app/undo/UndoCommand.cpp

    src/app/undo/UndoStack.cpp

    src/app/project/SceneDocument.cpp

    src/app/project/SceneSubtreeArchive.cpp

    src/app/project/ProjectChooser.cpp

    src/app/project/ProjectEditorState.cpp

    src/app/project/ProjectSession.cpp

    src/app/core/PlayModeController.cpp

    src/app/core/PhysicsWorld.cpp

    src/app/scene/SceneComponentCatalog.cpp

    src/app/project/SceneProjectIO.cpp

    src/app/panels/ProjectFilesPanel.cpp

    src/app/panels/LightingPanel.cpp
    src/app/scene/RenderDiagnostics.cpp

    src/app/editor/EditorWidgets.cpp

    src/app/panels/SceneHierarchyPanel.cpp

    src/app/panels/SceneInspectorPanel.cpp

    src/app/inspector/InspectorEditMode.cpp

    src/app/inspector/InspectorMultiEdit.cpp

    src/app/inspector/InspectorTransform.cpp

    src/app/panels/SceneToolbarPanel.cpp

    src/app/panels/SceneViewportPanel.cpp

    src/app/panels/GameViewportPanel.cpp

    src/app/scene/SceneEditor.cpp

    src/app/scene/SceneEditingController.cpp

    src/app/scene/Scene.cpp
    src/app/scene/SceneHierarchyOps.cpp
    src/app/scene/SceneImportService.cpp
    src/app/scene/SceneMeshLibrary.cpp
    src/app/scene/SceneObjectOperations.cpp
    src/app/scene/SceneObjectStore.cpp
    src/app/scene/SceneRenderer.cpp
    src/app/scene/SceneSelectionController.cpp
    src/app/scene/SceneSpawnService.cpp
    src/app/scene/SceneSpawnBuilders.cpp
    src/app/scene/SceneCamera.cpp

    src/engine/camera/Camera.cpp

    src/engine/platform/ImGuiLayer.cpp

    src/engine/platform/ImGuiFonts.cpp

    src/engine/platform/NativeProgressWindow.cpp

    src/engine/platform/Input.cpp

    src/engine/platform/InputDiagnostics.cpp
    src/engine/platform/EngineLog.cpp
    src/engine/platform/EngineDiagnostics.cpp
    src/engine/platform/ExceptionMessage.cpp
    src/engine/platform/FrameDiagnostics.cpp
    src/engine/platform/RenderPathDiagnostics.cpp

    src/engine/rhi/GfxContext.cpp

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
    src/engine/lighting/IrradianceSh.cpp

    src/engine/rendering/Material.cpp

    src/engine/rendering/Texture.cpp

    src/engine/assets/TextureCache.cpp

    src/engine/rendering/MaterialTextures.cpp

    src/engine/rendering/Mesh.cpp

    src/engine/rendering/GridRenderer.cpp

    src/engine/assets/ModelImporter.cpp
    src/engine/assets/TangentSpace.cpp

    src/engine/assets/StbImage.cpp

    src/engine/assets/ProjectAssets.cpp

    src/engine/assets/FileDialog.cpp

    src/engine/rendering/Renderer.cpp

    src/engine/rendering/Framebuffer.cpp

    src/engine/rendering/ScreenSpaceEffects.cpp

    src/engine/rendering/Shader.cpp

    src/engine/rendering/ShaderCache.cpp

    src/primitives/Cube.cpp

    src/primitives/PrimitiveMeshUtils.cpp

    src/primitives/Sphere.cpp

    src/primitives/Cylinder.cpp

    src/primitives/Capsule.cpp

    src/primitives/Plane.cpp

)



target_include_directories(game-engine PRIVATE vendor/stb vendor/IconFontCppHeaders src ${tinygltf_SOURCE_DIR})



target_link_libraries(game-engine PRIVATE
    glfw
    glm::glm
    imgui_lib
    imguizmo_lib
    nlohmann_json::nlohmann_json
    Jolt
    D3D12MemoryAllocator
    mikktspace
    d3d12
    dxgi
    dxguid
    dxcompiler
)

target_compile_definitions(game-engine PRIVATE NOMINMAX)

if(GAME_ENGINE_D3D12_DEBUG_LAYER)
    target_compile_definitions(game-engine PRIVATE GAME_ENGINE_D3D12_DEBUG_LAYER)
endif()

if(MSVC)
    target_link_options(game-engine PRIVATE "$<$<CONFIG:Debug>:/MAP:FULL>")
endif()

if(WIN32)
    target_link_libraries(game-engine PRIVATE comdlg32 shell32 ole32 comctl32)
endif()

# DXC is linked at build time but loaded at runtime — copy vendored or SDK redist DLLs next to the exe.
function(game_engine_copy_dxc_runtime target)
    if(NOT WIN32)
        return()
    endif()

    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_dxc_arch x64)
    else()
        set(_dxc_arch x86)
    endif()

    set(_dxc_redist_candidates
        "${CMAKE_SOURCE_DIR}/vendor/dxc/${_dxc_arch}")
    if(CMAKE_VS_WINDOWS_KITS_10_DIR AND CMAKE_VS_PLATFORM_NAME)
        list(APPEND _dxc_redist_candidates
            "${CMAKE_VS_WINDOWS_KITS_10_DIR}/Redist/D3D/${CMAKE_VS_PLATFORM_NAME}")
    endif()
    if(DEFINED ENV{WindowsSdkDir})
        get_filename_component(_sdk_root "$ENV{WindowsSdkDir}/.." ABSOLUTE)
        list(APPEND _dxc_redist_candidates "${_sdk_root}/Redist/D3D/${_dxc_arch}")
    endif()
    list(APPEND _dxc_redist_candidates
        "C:/Program Files (x86)/Windows Kits/10/Redist/D3D/${_dxc_arch}")

    set(_dxc_redist_dir "")
    foreach(_candidate IN LISTS _dxc_redist_candidates)
        if(EXISTS "${_candidate}/dxcompiler.dll" AND EXISTS "${_candidate}/dxil.dll")
            set(_dxc_redist_dir "${_candidate}")
            break()
        endif()
    endforeach()

    if(NOT _dxc_redist_dir)
        message(WARNING
            "Could not locate dxcompiler.dll + dxil.dll. "
            "Place them in vendor/dxc/${_dxc_arch}/ or install the Windows SDK D3D redist.")
        return()
    endif()

    message(STATUS "DXC runtime for ${target}: ${_dxc_redist_dir}")

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_dxc_redist_dir}/dxcompiler.dll"
            "$<TARGET_FILE_DIR:${target}>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_dxc_redist_dir}/dxil.dll"
            "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Copying DXC runtime (dxcompiler.dll, dxil.dll)")
endfunction()

game_engine_copy_dxc_runtime(game-engine)



add_custom_command(TARGET game-engine POST_BUILD

    COMMAND ${CMAKE_COMMAND} -E copy_directory

        "${CMAKE_SOURCE_DIR}/assets"

        "$<TARGET_FILE_DIR:game-engine>/assets"

    COMMENT "Copying assets to output directory")

if(MSVC)
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT game-engine)
endif()

enable_testing()

add_executable(engine-tests
    tests/shadow_map_math_test.cpp
    tests/irradiance_sh_test.cpp
    src/engine/lighting/ShadowMapMath.cpp
    src/engine/lighting/LightingProbe.cpp
    src/engine/lighting/IrradianceSh.cpp
    src/engine/platform/ExceptionMessage.cpp
    src/engine/platform/EngineDiagnostics.cpp
)

target_include_directories(engine-tests PRIVATE src)
target_link_libraries(engine-tests PRIVATE glm::glm)
add_test(NAME engine-tests COMMAND engine-tests)

add_executable(descriptor-heap-tests
    tests/descriptor_heap_test.cpp
    src/engine/rhi/d3d12/FixedDescriptorHeap.cpp
)

target_include_directories(descriptor-heap-tests PRIVATE src tests)
add_test(NAME descriptor-heap-tests COMMAND descriptor-heap-tests)

# GPU render integration tests (manual only).
# Heavy: creates a real D3D12 device/window and can run for a long time.
# Off by default so `cmake --build .` and `ctest` never pick it up accidentally.
# Enable explicitly: cmake -B build -DGAME_ENGINE_BUILD_D3D12_RENDER_TESTS=ON
# Run manually:      .\build\Debug\d3d12-render-tests.exe
option(GAME_ENGINE_BUILD_D3D12_RENDER_TESTS
    "Build d3d12-render-tests (GPU-heavy manual suite; never auto-run without user permission)"
    OFF)

if(WIN32 AND GAME_ENGINE_BUILD_D3D12_RENDER_TESTS)
    set(D3D12_RENDER_TEST_SOURCES
        tests/d3d12_render_tests.cpp
        tests/d3d12_test_harness.cpp
        src/engine/rhi/GfxContext.cpp
        src/engine/rhi/d3d12/FixedDescriptorHeap.cpp
        src/engine/rhi/d3d12/GpuBuffer.cpp
    src/engine/rhi/d3d12/HlslCompiler.cpp
        src/engine/rendering/Framebuffer.cpp
        src/engine/rendering/Shader.cpp
        src/engine/rendering/ShaderCache.cpp
        src/engine/rendering/Mesh.cpp
        src/engine/rendering/Material.cpp
        src/engine/rendering/MaterialTextures.cpp
        src/engine/rendering/Texture.cpp
        src/engine/assets/TextureCache.cpp
        src/engine/assets/StbImage.cpp
        src/engine/assets/TangentSpace.cpp
        src/engine/camera/Camera.cpp
        src/engine/platform/Input.cpp
        src/engine/lighting/SceneLighting.cpp
        src/engine/lighting/Light.cpp
        src/engine/lighting/IBL.cpp
        src/engine/lighting/IrradianceSh.cpp
        src/engine/lighting/CascadedShadowMap.cpp
        src/engine/lighting/ShadowMapMath.cpp
        src/engine/rendering/GridRenderer.cpp
        src/engine/gizmos/GizmoDraw.cpp
        src/engine/platform/FrameDiagnostics.cpp
        src/engine/platform/EngineDiagnostics.cpp
        src/engine/platform/EngineLog.cpp
        src/engine/platform/ExceptionMessage.cpp
    src/engine/platform/RenderPathDiagnostics.cpp
        src/engine/rendering/RenderDebug.cpp
        src/primitives/Cube.cpp
        src/primitives/Sphere.cpp
        src/primitives/PrimitiveMeshUtils.cpp
    )

    add_executable(d3d12-render-tests ${D3D12_RENDER_TEST_SOURCES})

    target_include_directories(d3d12-render-tests PRIVATE src tests vendor/stb ${json_SOURCE_DIR}/include)
    target_link_libraries(d3d12-render-tests PRIVATE
        glfw
        glm::glm
        imgui_lib
        mikktspace
        nlohmann_json::nlohmann_json
        D3D12MemoryAllocator
        d3d12
        dxgi
        dxguid
        dxcompiler
    )
    target_compile_definitions(d3d12-render-tests PRIVATE NOMINMAX)
    game_engine_copy_dxc_runtime(d3d12-render-tests)

    add_custom_command(TARGET d3d12-render-tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:d3d12-render-tests>/assets"
        COMMENT "Copying assets for d3d12-render-tests")

    # Intentionally NOT registered with CTest — run the .exe manually when needed.
endif()

