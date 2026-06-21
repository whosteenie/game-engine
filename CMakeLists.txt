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

add_executable(game-engine
    src/main.cpp
    src/app/Application.cpp
    src/app/DemoScene.cpp
    src/engine/Camera.cpp
    src/engine/Input.cpp
    src/engine/Light.cpp
    src/engine/SceneLighting.cpp
    src/engine/ShadowMap.cpp
    src/engine/Material.cpp
    src/engine/Mesh.cpp
    src/engine/Renderer.cpp
    src/engine/Shader.cpp
    src/engine/GridRenderer.cpp
    src/primitives/Cube.cpp
    src/primitives/Floor.cpp
    src/primitives/Triangle.cpp
    vendor/glad/src/glad.c
)

target_include_directories(game-engine PRIVATE vendor/glad/include src)

find_package(OpenGL REQUIRED)
target_link_libraries(game-engine PRIVATE glfw OpenGL::GL glm::glm)

add_custom_command(TARGET game-engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:game-engine>/assets"
    COMMENT "Copying assets to output directory")
