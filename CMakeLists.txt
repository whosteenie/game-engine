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

    GIT_TAG v1.91.6

)

FetchContent_MakeAvailable(imgui)



FetchContent_Declare(

    imguizmo

    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git

    GIT_TAG be8aa4aeab86b402701c8c1df011bd8cd776760b

)



FetchContent_GetProperties(imguizmo)

if(NOT imguizmo_POPULATED)

    FetchContent_Populate(imguizmo)

endif()



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

    src/app/DebugPanel.cpp

    src/app/SceneHierarchyPanel.cpp

    src/app/SceneEditor.cpp

    src/app/DemoScene.cpp

    src/engine/Camera.cpp

    src/engine/ImGuiLayer.cpp

    src/engine/Input.cpp

    src/engine/Light.cpp

    src/engine/SceneLighting.cpp

    src/engine/SceneObject.cpp

    src/engine/Transform.cpp

    src/engine/ScenePicker.cpp

    src/engine/SelectionRenderer.cpp

    src/engine/GizmoGeometry.cpp

    src/engine/ShadowMap.cpp

    src/engine/IBL.cpp

    src/engine/Material.cpp

    src/engine/Texture.cpp

    src/engine/TextureCache.cpp

    src/engine/MaterialTextures.cpp

    src/engine/Mesh.cpp

    src/engine/Renderer.cpp

    src/engine/Shader.cpp

    src/engine/GridRenderer.cpp

    src/engine/LightGizmoRenderer.cpp

    src/primitives/Cube.cpp

    src/primitives/Floor.cpp

    src/primitives/Triangle.cpp

    vendor/glad/src/glad.c

)



target_include_directories(game-engine PRIVATE vendor/glad/include vendor/stb src)



find_package(OpenGL REQUIRED)

target_link_libraries(game-engine PRIVATE glfw OpenGL::GL glm::glm imgui_lib imguizmo_lib)



add_custom_command(TARGET game-engine POST_BUILD

    COMMAND ${CMAKE_COMMAND} -E copy_directory

        "${CMAKE_SOURCE_DIR}/assets"

        "$<TARGET_FILE_DIR:game-engine>/assets"

    COMMENT "Copying assets to output directory")

