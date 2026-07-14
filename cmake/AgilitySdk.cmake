# PF5: the DXR 1.2-capable runtime is an app-local, versioned dependency. Keep the SDK outside
# the source tree's binary ignore rules; CI/release provisioning unpacks this exact NuGet package
# to the path below before configuring CMake.
set(GAME_ENGINE_AGILITY_SDK_PACKAGE_VERSION "1.619.4" CACHE STRING "Pinned DirectX 12 Agility SDK package")
set(GAME_ENGINE_AGILITY_SDK_LOADER_VERSION "619" CACHE STRING "D3D12SDKVersion exported by the pinned SDK")
set(
    GAME_ENGINE_AGILITY_SDK_ROOT
    "${CMAKE_SOURCE_DIR}/vendor/agility-sdk/${GAME_ENGINE_AGILITY_SDK_PACKAGE_VERSION}"
    CACHE PATH
    "Extracted Microsoft.Direct3D.D3D12 package root")

set(_game_engine_agility_bin "${GAME_ENGINE_AGILITY_SDK_ROOT}/build/native/bin/x64")
set(_game_engine_agility_available FALSE)
if(WIN32 AND EXISTS "${_game_engine_agility_bin}/D3D12Core.dll")
    set(_game_engine_agility_available TRUE)
endif()

option(
    GAME_ENGINE_USE_AGILITY_SDK
    "Package the pinned app-local DirectX 12 Agility SDK when it is provisioned"
    ${_game_engine_agility_available})

function(game_engine_enable_agility_sdk target)
    if(NOT WIN32 OR NOT GAME_ENGINE_USE_AGILITY_SDK)
        return()
    endif()

    if(NOT EXISTS "${_game_engine_agility_bin}/D3D12Core.dll")
        message(FATAL_ERROR
            "GAME_ENGINE_USE_AGILITY_SDK=ON requires Microsoft.Direct3D.D3D12 "
            "${GAME_ENGINE_AGILITY_SDK_PACKAGE_VERSION} extracted at ${GAME_ENGINE_AGILITY_SDK_ROOT}. "
            "Expected build/native/bin/x64/D3D12Core.dll.")
    endif()

    target_compile_definitions(
        ${target}
        PRIVATE GAME_ENGINE_AGILITY_SDK_VERSION=${GAME_ENGINE_AGILITY_SDK_LOADER_VERSION})

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/D3D12"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_game_engine_agility_bin}/D3D12Core.dll"
            "$<TARGET_FILE_DIR:${target}>/D3D12/D3D12Core.dll"
        COMMENT "Copying pinned DirectX 12 Agility SDK runtime")

    # The debug layer must match D3D12Core. It is absent in some retail package layouts, so copy
    # it when supplied without making release packaging depend on it.
    if(EXISTS "${_game_engine_agility_bin}/d3d12SDKLayers.dll")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_game_engine_agility_bin}/d3d12SDKLayers.dll"
                "$<TARGET_FILE_DIR:${target}>/D3D12/d3d12SDKLayers.dll")
    endif()

    message(STATUS
        "Using DirectX 12 Agility SDK ${GAME_ENGINE_AGILITY_SDK_PACKAGE_VERSION} "
        "(D3D12SDKVersion ${GAME_ENGINE_AGILITY_SDK_LOADER_VERSION}) for ${target}")
endfunction()
