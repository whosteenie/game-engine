cmake_minimum_required(VERSION 3.20)

project(game-engine VERSION 0.1.0 LANGUAGES CXX)



set(CMAKE_CXX_STANDARD 17)

set(CMAKE_CXX_STANDARD_REQUIRED ON)



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



add_library(imgui_lib STATIC

    ${imgui_SOURCE_DIR}/imgui.cpp

    ${imgui_SOURCE_DIR}/imgui_draw.cpp

    ${imgui_SOURCE_DIR}/imgui_tables.cpp

    ${imgui_SOURCE_DIR}/imgui_widgets.cpp

    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp

    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp

)



target_include_directories(imgui_lib PUBLIC

    ${imgui_SOURCE_DIR}

    ${imgui_SOURCE_DIR}/backends)



target_compile_definitions(imgui_lib PUBLIC IMGUI_IMPL_OPENGL_LOADER_GLAD)

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
    src/app/scene/SceneCamera.cpp

    src/engine/camera/Camera.cpp

    src/engine/platform/ImGuiLayer.cpp

    src/engine/platform/ImGuiFonts.cpp

    src/engine/platform/NativeProgressWindow.cpp

    src/engine/platform/Input.cpp

    src/engine/lighting/Light.cpp

    src/engine/components/LightComponent.cpp
    src/engine/components/CameraComponent.cpp

    src/engine/components/ColliderComponent.cpp

    src/engine/components/RigidBodyComponent.cpp

    src/engine/lighting/SceneLighting.cpp

    src/engine/scene/InspectorComponentOrder.cpp
    src/engine/scene/SceneObject.cpp

    src/engine/scene/Transform.cpp

    src/engine/scene/ScenePicker.cpp

    src/engine/scene/SceneHierarchy.cpp

    src/engine/scene/ScenePrimitive.cpp

    src/engine/gizmos/SelectionRenderer.cpp

    src/engine/gizmos/GizmoGeometry.cpp

    src/engine/lighting/ShadowMap.cpp
    src/engine/lighting/CascadedShadowMap.cpp
    src/engine/lighting/ShadowMapMath.cpp
    src/engine/rendering/RenderDebug.cpp

    src/engine/lighting/IBL.cpp

    src/engine/rendering/Material.cpp

    src/engine/rendering/Texture.cpp

    src/engine/assets/TextureCache.cpp

    src/engine/rendering/MaterialTextures.cpp

    src/engine/rendering/Mesh.cpp

    src/engine/assets/ModelImporter.cpp

    src/engine/assets/ProjectAssets.cpp

    src/engine/assets/FileDialog.cpp

    src/engine/rendering/Renderer.cpp

    src/engine/rendering/Framebuffer.cpp

    src/engine/rendering/ScreenSpaceEffects.cpp

    src/engine/rendering/Shader.cpp

    src/engine/rendering/ShaderCache.cpp

    src/engine/rendering/GridRenderer.cpp

    src/engine/gizmos/CameraGizmoRenderer.cpp

    src/engine/gizmos/ColliderGizmoRenderer.cpp

    src/engine/gizmos/LightGizmoRenderer.cpp

    src/primitives/Cube.cpp

    src/primitives/PrimitiveMeshUtils.cpp

    src/primitives/Sphere.cpp

    src/primitives/Cylinder.cpp

    src/primitives/Capsule.cpp

    src/primitives/Plane.cpp

    vendor/glad/src/glad.c

)



target_include_directories(game-engine PRIVATE vendor/glad/include vendor/stb vendor/IconFontCppHeaders src ${tinygltf_SOURCE_DIR})



find_package(OpenGL REQUIRED)

target_link_libraries(game-engine PRIVATE glfw OpenGL::GL glm::glm imgui_lib imguizmo_lib nlohmann_json::nlohmann_json Jolt)

target_compile_definitions(game-engine PRIVATE NOMINMAX)

if(WIN32)
    target_link_libraries(game-engine PRIVATE comdlg32 shell32 ole32 comctl32)
endif()



add_custom_command(TARGET game-engine POST_BUILD

    COMMAND ${CMAKE_COMMAND} -E copy_directory

        "${CMAKE_SOURCE_DIR}/assets"

        "$<TARGET_FILE_DIR:game-engine>/assets"

    COMMENT "Copying assets to output directory")

enable_testing()

add_executable(engine-tests
    tests/shadow_map_math_test.cpp
    src/engine/lighting/ShadowMapMath.cpp
)

target_include_directories(engine-tests PRIVATE src)
target_link_libraries(engine-tests PRIVATE glm::glm)
add_test(NAME engine-tests COMMAND engine-tests)

