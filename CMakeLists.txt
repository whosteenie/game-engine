cmake_minimum_required(VERSION 3.20)

project(game-engine VERSION 0.1.0 LANGUAGES CXX C)



set(CMAKE_CXX_STANDARD 17)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    # Allow parallel CL.exe invocations (MSBuild /m) without vc143.pdb lock failures.
    add_compile_options(/FS)

    # MSBuild /m schedules projects, but does not parallelize translation units within one project.
    # Keep enough capacity free for an interactive workstation while allowing an explicit CI/user
    # override. Zero retains MSVC's unbounded /MP behavior.
    set(GAME_ENGINE_COMPILE_JOBS "" CACHE STRING
        "Maximum MSVC compiler processes per project; empty selects half the logical CPUs (4-12), 0 uses unbounded /MP")
    if(GAME_ENGINE_COMPILE_JOBS STREQUAL "")
        cmake_host_system_information(
            RESULT GAME_ENGINE_EFFECTIVE_COMPILE_JOBS
            QUERY NUMBER_OF_LOGICAL_CORES)
        math(EXPR GAME_ENGINE_EFFECTIVE_COMPILE_JOBS
            "${GAME_ENGINE_EFFECTIVE_COMPILE_JOBS} / 2")
        if(GAME_ENGINE_EFFECTIVE_COMPILE_JOBS LESS 4)
            set(GAME_ENGINE_EFFECTIVE_COMPILE_JOBS 4)
        elseif(GAME_ENGINE_EFFECTIVE_COMPILE_JOBS GREATER 12)
            set(GAME_ENGINE_EFFECTIVE_COMPILE_JOBS 12)
        endif()
    elseif(GAME_ENGINE_COMPILE_JOBS MATCHES "^[0-9]+$")
        set(GAME_ENGINE_EFFECTIVE_COMPILE_JOBS "${GAME_ENGINE_COMPILE_JOBS}")
    else()
        message(FATAL_ERROR
            "GAME_ENGINE_COMPILE_JOBS must be empty or a non-negative integer, got: "
            "${GAME_ENGINE_COMPILE_JOBS}")
    endif()
    message(STATUS "MSVC compiler processes per project: ${GAME_ENGINE_EFFECTIVE_COMPILE_JOBS}")

    # Apply the bounded /MP option explicitly to repository-owned targets; fetched dependency
    # targets remain untouched.
    function(game_engine_enable_msvc_parallel_compile target)
        if(GAME_ENGINE_EFFECTIVE_COMPILE_JOBS EQUAL 0)
            target_compile_options(${target} PRIVATE /MP)
        else()
            target_compile_options(${target} PRIVATE "/MP${GAME_ENGINE_EFFECTIVE_COMPILE_JOBS}")
        endif()
    endfunction()
else()
    function(game_engine_enable_msvc_parallel_compile target)
    endfunction()
endif()

option(GAME_ENGINE_D3D12_DEBUG_LAYER "Enable the D3D12 debug layer (Debug builds only; see devdoc/dxr/shader-infrastructure.md)" OFF)

# NVIDIA DLSS via Streamline (devdoc/rendering/dlss-super-resolution.md, Phase S0). Additive upscaler/AA;
# NVIDIA-only, gracefully hidden on unsupported hardware. Prebuilt SL/DLSS DLLs ship in the
# release zip only (removed from the git repo as of SL 2.7.32), so we fetch the release artifact.
option(GAME_ENGINE_ENABLE_DLSS "Enable NVIDIA DLSS Super Resolution via Streamline (Windows/NVIDIA)" ON)



include(FetchContent)

# Several deps are download-only (no add_subdirectory). MakeAvailable would configure their
# CMakeLists.txt; single-arg Populate after Declare is the supported pattern under CMP0169 OLD.
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

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

    GIT_TAG 1.0.3

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

# NVIDIA NRD (DXR Phase D5, devdoc/dxr/nrd-integration.md). Pinned to the release whose API
# the integration was written against (v4.17). NRD fetches its own dependencies (ShaderMake,
# MathLib) and compiles its internal shaders at build time; DXIL blobs are embedded in the
# static lib. If the build cannot locate a dxc.exe for ShaderMake, set SHADERMAKE_DXC_PATH.
set(NRD_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
# Guide-buffer encoding contract — must match assets/shaders/dxr/reflections.hlsl:
#   NRD_NORMAL_ENCODING 3 = RGBA16_UNORM: rgb = worldNormal * 0.5 + 0.5, a = roughness
#   (same packing math as RGBA8, 16-bit precision — 8-bit normals quantize on curved
#   surfaces and break RELAX's normal edge-stopping, reading as blur bands/shimmer)
#   NRD_ROUGHNESS_ENCODING 1 = linear roughness
set(NRD_NORMAL_ENCODING "3" CACHE STRING "" FORCE)
set(NRD_ROUGHNESS_ENCODING "1" CACHE STRING "" FORCE)
set(NRD_EMBEDS_SPIRV_SHADERS OFF CACHE BOOL "" FORCE) # D3D12-only engine
set(NRD_EMBEDS_DXBC_SHADERS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(

    nrd

    GIT_REPOSITORY https://github.com/NVIDIA-RTX/NRD.git

    GIT_TAG v4.17.3 # latest tagged release; master reports 4.17.4 but is untagged

)

FetchContent_GetProperties(nrd)
if(NOT nrd_POPULATED)
    FetchContent_Populate(nrd)
endif()

# NRD 4.17.3 declares its embedded-shader target without outputs, so Visual Studio runs all 159
# DXIL tasks on every build and then relinks every downstream target. Apply a pinned, idempotent
# project-owned patch before adding NRD; the patch fails if the expected upstream block changes.
execute_process(
    COMMAND ${CMAKE_COMMAND}
        "-DNRD_SOURCE_DIR=${nrd_SOURCE_DIR}"
        -P "${CMAKE_SOURCE_DIR}/cmake/PatchNrdIncremental.cmake"
    RESULT_VARIABLE _nrd_patch_result)
if(NOT _nrd_patch_result EQUAL 0)
    message(FATAL_ERROR "Failed to apply the NRD incremental shader patch")
endif()
add_subdirectory("${nrd_SOURCE_DIR}" "${nrd_BINARY_DIR}")

# NVIDIA Streamline (DLSS SR, devdoc/rendering/dlss-super-resolution.md Phase S0). Pinned to v2.12.0. The
# release zip carries headers (include/), the import lib (lib/x64/sl.interposer.lib), and the
# prebuilt signed plugin DLLs (bin/x64) that are NOT in the git repo. We only download+extract it
# (no add_subdirectory — we consume prebuilt artifacts), then resolve the real paths by locating
# sl.h and sl.interposer.lib so nested-zip layouts are handled without guessing.
set(GAME_ENGINE_HAS_DLSS OFF)
if(GAME_ENGINE_ENABLE_DLSS AND WIN32)
    FetchContent_Declare(
        streamline
        URL https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.12.0/streamline-sdk-v2.12.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_GetProperties(streamline)
    if(NOT streamline_POPULATED)
        FetchContent_Populate(streamline)
    endif()

    file(GLOB_RECURSE _sl_header "${streamline_SOURCE_DIR}/sl.h" "${streamline_SOURCE_DIR}/*/sl.h")
    file(GLOB_RECURSE _sl_interposer_dll "${streamline_SOURCE_DIR}/sl.interposer.dll" "${streamline_SOURCE_DIR}/*/sl.interposer.dll")

    # Prefer the signed PRODUCTION plugin DLLs (bin/x64) over the DEVELOPMENT variants
    # (bin/x64/development): the dev DLLs do heavy per-plugin validation/logging that adds seconds to
    # startup and floods the log. Fall back to whatever was found if only one set ships.
    set(_sl_dll0 "")
    foreach(_cand IN LISTS _sl_interposer_dll)
        if(NOT _cand MATCHES "/development/")
            set(_sl_dll0 "${_cand}")
            break()
        endif()
    endforeach()
    if(NOT _sl_dll0 AND _sl_interposer_dll)
        list(GET _sl_interposer_dll 0 _sl_dll0)
    endif()

    if(_sl_header AND _sl_dll0)
        list(GET _sl_header 0 _sl_header0)
        get_filename_component(STREAMLINE_INCLUDE_DIR "${_sl_header0}" DIRECTORY)
        get_filename_component(STREAMLINE_BIN_DIR "${_sl_dll0}" DIRECTORY)
        set(GAME_ENGINE_HAS_DLSS ON)
        message(STATUS "Streamline SDK: include=${STREAMLINE_INCLUDE_DIR} bin=${STREAMLINE_BIN_DIR}")
    else()
        message(WARNING
            "GAME_ENGINE_ENABLE_DLSS is ON but the Streamline SDK layout was not found under "
            "${streamline_SOURCE_DIR} (need sl.h and sl.interposer.dll). DLSS will be compiled out.")
    endif()
endif()



# Vendored ImGuizmo (based on CedricGuillemet/ImGuizmo@be8aa4a) with edge-on
# rotation screen-space drag. See vendor/imguizmo for LICENSE.
set(imguizmo_SOURCE_DIR "${CMAKE_SOURCE_DIR}/vendor/imguizmo")

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
if(MSVC)
    # Jolt 5.3.0 hard-codes unbounded /MP in its top-level CMAKE_CXX_FLAGS. A fresh build reached
    # 27 cl.exe processes even after project-owned targets were capped, so apply the same explicit
    # per-project limit to this one measured dependency without changing its fetched sources.
    game_engine_enable_msvc_parallel_compile(Jolt)
endif()

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



include("${CMAKE_SOURCE_DIR}/cmake/AgilitySdk.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/EngineLibraries.cmake")

set(GAME_ENGINE_APP_SOURCES
    src/main.cpp

    src/app/core/AgilitySdkExports.cpp
    src/app/core/Application.cpp
    src/app/core/AutomatedBenchmarkCapture.cpp

    src/app/editor/EditorSettings.cpp
    src/app/editor/TuningSectionState.cpp
    src/app/editor/SettingRegistry.cpp

    src/app/editor/EditorDockSpace.cpp
    src/app/editor/EditorDockLayout.cpp
    src/app/editor/MainMenuBar.cpp
    src/app/editor/EditorTopToolbar.cpp
    src/app/editor/EditorClipboard.cpp
    src/app/editor/EditorMouseWrapping.cpp

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
    src/app/panels/lighting/LightingPanelShared.cpp
    src/app/panels/lighting/LightingPanelUi.cpp
    src/app/panels/lighting/LightingPanelWidgets.cpp
    src/app/panels/lighting/SceneSection.cpp
    src/app/panels/lighting/EnvironmentSection.cpp
    src/app/panels/lighting/DirectionalShadowsSection.cpp
    src/app/panels/lighting/PostProcessingSection.cpp
    src/app/panels/lighting/ScreenSpaceSection.cpp
    src/app/panels/lighting/AntiAliasingSection.cpp
    src/app/panels/lighting/TextureFilteringSection.cpp
    src/app/panels/lighting/SsgiSection.cpp
    src/app/panels/lighting/SsrSection.cpp
    src/app/panels/lighting/RayTracingSection.cpp
    src/app/panels/lighting/DiagnosticsSection.cpp
    src/app/panels/PerformancePanel.cpp
    src/app/scene/RenderDiagnostics.cpp

    src/app/editor/EditorWidgets.cpp
    src/app/editor/OffscreenViewportPanel.cpp

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
    src/app/scene/GpuScene.cpp
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
)

add_executable(game-engine ${GAME_ENGINE_APP_SOURCES})
game_engine_enable_msvc_parallel_compile(game-engine)

target_include_directories(game-engine PRIVATE
    vendor/stb
    vendor/IconFontCppHeaders
    src
    ${tinygltf_SOURCE_DIR})

target_link_libraries(game-engine PRIVATE
    engine-render
    engine-dxr
    imgui_lib
    imguizmo_lib
    nlohmann_json::nlohmann_json
    Jolt
)

game_engine_enable_agility_sdk(game-engine)

# Streamline (DLSS SR, Phase S0): headers only + copy the prebuilt SL/DLSS plugin DLLs next to the
# exe. We do NOT statically link sl.interposer.lib — it exports DXGI/D3D entry points that would
# clash with our dxgi.lib/d3d12.lib and interpose the whole device. Instead DlssContext loads
# sl.interposer.dll dynamically (NVIDIA's recommended least-invasive path for SR-only), so nothing
# changes for the non-DLSS pipeline.
if(GAME_ENGINE_HAS_DLSS)
    target_include_directories(game-engine PRIVATE "${STREAMLINE_INCLUDE_DIR}")

    # DLSS Super Resolution + Ray Reconstruction runtime set. We copy only these plugins (not
    # reflex, dlss_g, nis, imgui, deepdvc, directsr, …) — copying every plugin makes SL map+ignore
    # each at init (log spam + slower startup). RR (dxr-dlss-rr.md) adds sl.dlss_d.dll (the RR
    # plugin) + nvngx_dlssd.dll (the RR NGX model); without them kFeatureDLSS_RR fails to load.
    set(_sl_runtime_dlls
        "${STREAMLINE_BIN_DIR}/sl.interposer.dll"
        "${STREAMLINE_BIN_DIR}/sl.common.dll"
        "${STREAMLINE_BIN_DIR}/sl.dlss.dll"
        "${STREAMLINE_BIN_DIR}/nvngx_dlss.dll"
        "${STREAMLINE_BIN_DIR}/sl.dlss_d.dll"
        "${STREAMLINE_BIN_DIR}/nvngx_dlssd.dll")
    foreach(_sl_dll IN LISTS _sl_runtime_dlls)
        if(EXISTS "${_sl_dll}")
            add_custom_command(TARGET game-engine POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_sl_dll}" "$<TARGET_FILE_DIR:game-engine>"
                COMMENT "Copying Streamline runtime: ${_sl_dll}")
        endif()
    endforeach()
endif()

if(MSVC)
    target_link_options(game-engine PRIVATE "$<$<CONFIG:Debug>:/MAP:FULL>")
    target_link_options(game-engine PRIVATE /NODEFAULTLIB:libcmt)
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
    tests/engine_tests_main.cpp
    tests/shadow_map_math_test.cpp
    tests/irradiance_sh_test.cpp
    tests/color_space_test.cpp
    tests/rotation_utils_test.cpp
    tests/dxr_settings_test.cpp
    tests/directional_shadow_settings_test.cpp
    tests/dxr_acceleration_structure_test.cpp
    tests/pt_temporal_history_test.cpp
    tests/path_rng_test.cpp
    tests/restir_types_test.cpp
    tests/restir_wrs_test.cpp
    tests/restir_gi_test.cpp
    tests/restir_di_test.cpp
    tests/restir_surface_validation_test.cpp
    tests/dxr_shader_infrastructure_test.cpp
    tests/material_test.cpp
    tests/guide_encoding_test.cpp
    tests/payload_pack_test.cpp
    tests/restir_temporal_chain_test.cpp
    tests/refraction_test.cpp
    tests/brdf_energy_test.cpp
    tests/mis_nee_test.cpp
    tests/env_importance_test.cpp
    tests/nee_mis_integration_test.cpp
    tests/frustum_cull_test.cpp
    tests/capture_manifest_test.cpp
    tests/temporal_camera_packet_test.cpp
    tests/dlss_frame_token_test.cpp
    tests/dlss_extent_planner_test.cpp
    tests/reconstruction_exposure_test.cpp
    tests/reconstruction_jitter_test.cpp
    tests/history_compatibility_test.cpp
    tests/viewport_resize_stabilizer_test.cpp
    tests/project_viewport_reveal_gate_test.cpp
    tests/background_work_test.cpp
    tests/pt_viewport_ownership_test.cpp
    tests/shader_compile_test.cpp
)
game_engine_enable_msvc_parallel_compile(engine-tests)

target_include_directories(engine-tests PRIVATE src tests)
target_link_libraries(engine-tests PRIVATE engine-render)
if(MSVC)
    target_link_options(engine-tests PRIVATE /NODEFAULTLIB:libcmt)
endif()
game_engine_copy_dxc_runtime(engine-tests)

add_custom_command(TARGET engine-tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:engine-tests>/assets"
    COMMENT "Copying assets for engine-tests")

add_test(NAME engine-tests COMMAND engine-tests)
set_tests_properties(engine-tests PROPERTIES
    LABELS "cpu"
    WORKING_DIRECTORY $<TARGET_FILE_DIR:engine-tests>)

add_executable(descriptor-heap-tests
    tests/descriptor_heap_test.cpp
    src/engine/rhi/d3d12/FixedDescriptorHeap.cpp
)
game_engine_enable_msvc_parallel_compile(descriptor-heap-tests)

target_include_directories(descriptor-heap-tests PRIVATE src tests)
add_test(NAME descriptor-heap-tests COMMAND descriptor-heap-tests)
set_tests_properties(descriptor-heap-tests PROPERTIES LABELS "cpu")

# GPU render integration tests (manual only).
# Heavy: creates a real D3D12 device/window and can run for a long time.
# Off by default so `cmake --build .` and `ctest` never pick it up accidentally.
# Enable explicitly: cmake -B build -DGAME_ENGINE_BUILD_D3D12_RENDER_TESTS=ON
# Run manually:      .\build\Debug\d3d12-render-tests.exe
option(GAME_ENGINE_BUILD_D3D12_RENDER_TESTS
    "Build d3d12-render-tests (GPU-heavy manual suite; never auto-run without user permission)"
    OFF)

if(WIN32 AND GAME_ENGINE_BUILD_D3D12_RENDER_TESTS)
    add_executable(d3d12-render-tests
        tests/d3d12_render_tests.cpp
        tests/d3d12_test_harness.cpp
        tests/d3d12_test_runner.cpp
        tests/pt_test_harness.cpp
    )
    game_engine_enable_msvc_parallel_compile(d3d12-render-tests)

    target_include_directories(d3d12-render-tests PRIVATE tests ${json_SOURCE_DIR}/include)
    target_link_libraries(d3d12-render-tests PRIVATE
        engine-dxr
        imgui_lib
        nlohmann_json::nlohmann_json
    )
    if(MSVC)
        target_link_options(d3d12-render-tests PRIVATE /NODEFAULTLIB:libcmt)
    endif()
    game_engine_copy_dxc_runtime(d3d12-render-tests)

    add_custom_command(TARGET d3d12-render-tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:d3d12-render-tests>/assets"
        COMMENT "Copying assets for d3d12-render-tests")

    # GPU tests are opt-in (require D3D12 device). Default CTest only runs cpu label.
    add_test(NAME gpu-smoke COMMAND d3d12-render-tests --tier=1)
    set_tests_properties(gpu-smoke PROPERTIES
        LABELS "gpu;gpu-smoke"
        TIMEOUT 120
        WORKING_DIRECTORY $<TARGET_FILE_DIR:d3d12-render-tests>)

    add_test(NAME gpu-pbr COMMAND d3d12-render-tests --tier=2)
    set_tests_properties(gpu-pbr PROPERTIES
        LABELS "gpu;gpu-pbr"
        TIMEOUT 180
        WORKING_DIRECTORY $<TARGET_FILE_DIR:d3d12-render-tests>)

    add_test(NAME gpu-dxr COMMAND d3d12-render-tests --tier=4)
    set_tests_properties(gpu-dxr PROPERTIES
        LABELS "gpu;gpu-dxr"
        TIMEOUT 180
        WORKING_DIRECTORY $<TARGET_FILE_DIR:d3d12-render-tests>)

    add_test(NAME gpu-dxr-pt COMMAND d3d12-render-tests --tier=5)
    set_tests_properties(gpu-dxr-pt PROPERTIES
        LABELS "gpu;gpu-dxr-pt"
        TIMEOUT 300
        WORKING_DIRECTORY $<TARGET_FILE_DIR:d3d12-render-tests>)
endif()
