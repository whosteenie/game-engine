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

    src/app/Application.cpp

    src/app/EditorSettings.cpp


    src/app/EditorDockSpace.cpp

    src/app/EditorDockLayout.cpp

    src/app/MainMenuBar.cpp
    src/app/EditorTopToolbar.cpp

    src/app/EditorClipboard.cpp

    src/app/UndoCommand.cpp

    src/app/UndoStack.cpp

    src/app/SceneDocument.cpp

    src/app/SceneSubtreeArchive.cpp

    src/app/ProjectChooser.cpp

    src/app/ProjectEditorState.cpp

    src/app/ProjectSession.cpp

    src/app/PlayModeController.cpp

    src/app/PhysicsWorld.cpp

    src/app/SceneComponentCatalog.cpp

    src/app/SceneProjectIO.cpp

    src/app/ProjectFilesPanel.cpp

    src/app/LightingPanel.cpp
    src/app/RenderDiagnostics.cpp

    src/app/EditorWidgets.cpp

    src/app/SceneHierarchyPanel.cpp

    src/app/SceneInspectorPanel.cpp

    src/app/InspectorEditMode.cpp

    src/app/InspectorMultiEdit.cpp

    src/app/InspectorTransform.cpp

    src/app/SceneToolbarPanel.cpp

    src/app/SceneViewportPanel.cpp

    src/app/GameViewportPanel.cpp

    src/app/SceneEditor.cpp

    src/app/Scene.cpp
    src/app/SceneCamera.cpp

    src/engine/Camera.cpp

    src/engine/ImGuiLayer.cpp

    src/engine/ImGuiFonts.cpp

    src/engine/NativeProgressWindow.cpp

    src/engine/Input.cpp

    src/engine/Light.cpp

    src/engine/LightComponent.cpp
    src/engine/CameraComponent.cpp

    src/engine/ColliderComponent.cpp

    src/engine/RigidBodyComponent.cpp

    src/engine/SceneLighting.cpp

    src/engine/InspectorComponentOrder.cpp
    src/engine/SceneObject.cpp

    src/engine/Transform.cpp

    src/engine/ScenePicker.cpp

    src/engine/SceneHierarchy.cpp

    src/engine/ScenePrimitive.cpp

    src/engine/SelectionRenderer.cpp

    src/engine/GizmoGeometry.cpp

    src/engine/ShadowMap.cpp
    src/engine/CascadedShadowMap.cpp
    src/engine/ShadowMapMath.cpp
    src/engine/RenderDebug.cpp

    src/engine/IBL.cpp

    src/engine/Material.cpp

    src/engine/Texture.cpp

    src/engine/TextureCache.cpp

    src/engine/MaterialTextures.cpp

    src/engine/Mesh.cpp

    src/engine/ModelImporter.cpp

    src/engine/ProjectAssets.cpp

    src/engine/FileDialog.cpp

    src/engine/Renderer.cpp

    src/engine/Framebuffer.cpp

    src/engine/ScreenSpaceEffects.cpp

    src/engine/Shader.cpp

    src/engine/ShaderCache.cpp

    src/engine/GridRenderer.cpp

    src/engine/CameraGizmoRenderer.cpp

    src/engine/ColliderGizmoRenderer.cpp

    src/engine/LightGizmoRenderer.cpp

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
    src/engine/ShadowMapMath.cpp
)

target_include_directories(engine-tests PRIVATE src)
target_link_libraries(engine-tests PRIVATE glm::glm)
add_test(NAME engine-tests COMMAND engine-tests)

