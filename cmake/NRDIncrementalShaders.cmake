# This file executes in NRD's directory scope after the NRD and ShaderMake targets are created.
# Keep dependencies conservative: stale embedded shader bytecode is worse than extra regeneration.

file(GLOB_RECURSE SHADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/*.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/*.hlsli")
set(NRD_SHADER_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Shaders.cfg")
list(APPEND SHADERS "${NRD_SHADER_CONFIG}")
set_source_files_properties(${SHADERS} PROPERTIES VS_TOOL_OVERRIDE "None")

get_target_property(ML_SOURCE_DIR MathLib SOURCE_DIR)
file(GLOB_RECURSE NRD_MATHLIB_SHADER_INPUTS CONFIGURE_DEPENDS
    "${ML_SOURCE_DIR}/*.h"
    "${ML_SOURCE_DIR}/*.hlsli")

set(SHADERMAKE_GENERAL_ARGS
    --force
    --flatten
    --stripReflection
    --WX
    --sRegShift ${SPIRV_SREG_OFFSET}
    --bRegShift ${SPIRV_BREG_OFFSET}
    --uRegShift ${SPIRV_UREG_OFFSET}
    --tRegShift ${SPIRV_TREG_OFFSET}
    --headerBlob
    --allResourcesBound
    --vulkanVersion 1.2
    --sourceDir "Shaders"
    --ignoreConfigDir
    -c "Shaders/Shaders.cfg"
    -o "${NRD_SHADERS_PATH}"
    -I "${ML_SOURCE_DIR}"
    -D NRD_INTERNAL)

if(SHADERMAKE_PATH)
    list(APPEND SHADERMAKE_GENERAL_ARGS --project "NRD" --compactProgress)
else()
    set(SHADERMAKE_PATH "ShaderMake")
    set(SHADERMAKE_DXC_PATH ${DXC_PATH})
    set(SHADERMAKE_DXC_VK_PATH ${DXC_SPIRV_PATH})
    set(SHADERMAKE_FXC_PATH ${FXC_PATH})
endif()

set(NRD_SHADER_COMMANDS)
set(NRD_SHADER_OUTPUTS)
set(NRD_SHADER_COMPILERS)
set(NRD_SHADER_PLATFORMS)
if(NRD_EMBEDS_DXIL_SHADERS)
    list(APPEND NRD_SHADER_COMMANDS
        COMMAND ${SHADERMAKE_PATH} -p DXIL --compiler "${SHADERMAKE_DXC_PATH}" ${SHADERMAKE_GENERAL_ARGS})
    list(APPEND NRD_SHADER_COMPILERS "${SHADERMAKE_DXC_PATH}")
    list(APPEND NRD_SHADER_PLATFORMS "DXIL")
    message(STATUS "NRD_EMBEDS_DXIL_SHADERS")
endif()
if(NRD_EMBEDS_SPIRV_SHADERS)
    list(APPEND NRD_SHADER_COMMANDS
        COMMAND ${SHADERMAKE_PATH} -p SPIRV --compiler "${SHADERMAKE_DXC_VK_PATH}" ${SHADERMAKE_GENERAL_ARGS})
    list(APPEND NRD_SHADER_COMPILERS "${SHADERMAKE_DXC_VK_PATH}")
    list(APPEND NRD_SHADER_PLATFORMS "SPIRV")
    message(STATUS "NRD_EMBEDS_SPIRV_SHADERS")
endif()
if(NRD_EMBEDS_DXBC_SHADERS)
    list(APPEND NRD_SHADER_COMMANDS
        COMMAND ${SHADERMAKE_PATH} -p DXBC --compiler "${SHADERMAKE_FXC_PATH}" ${SHADERMAKE_GENERAL_ARGS})
    list(APPEND NRD_SHADER_COMPILERS "${SHADERMAKE_FXC_PATH}")
    list(APPEND NRD_SHADER_PLATFORMS "DXBC")
    message(STATUS "NRD_EMBEDS_DXBC_SHADERS")
endif()

# ShaderMake emits one container header per HLSL entry point and platform. Declaring every header as
# an output makes deletion of any generated container invalidate the command.
file(GLOB NRD_SHADER_ENTRY_POINTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/*.hlsl")
foreach(_shader IN LISTS NRD_SHADER_ENTRY_POINTS)
    get_filename_component(_shader_name "${_shader}" NAME)
    string(REGEX REPLACE "\\.hlsl$" "" _shader_name "${_shader_name}")
    foreach(_platform IN LISTS NRD_SHADER_PLATFORMS)
        string(TOLOWER "${_platform}" _platform_lower)
        list(APPEND NRD_SHADER_OUTPUTS "${NRD_SHADERS_PATH}/${_shader_name}.${_platform_lower}.h")
    endforeach()
endforeach()

# Track ShaderMake source as well as its executable. ShaderMake is an ExternalProject in this NRD
# release, so rebuilding its nested solution before regeneration ensures a touched tool source is
# reflected in the executable rather than merely causing shaders to rerun with an old binary.
get_target_property(_shadermake_source_dir ShaderMake _EP_SOURCE_DIR)
get_target_property(_shadermake_binary_dir ShaderMake _EP_BINARY_DIR)
set(NRD_SHADERMAKE_INPUTS)
set(NRD_SHADERMAKE_REBUILD_COMMAND)
if(_shadermake_source_dir AND NOT _shadermake_source_dir MATCHES "NOTFOUND")
    file(GLOB_RECURSE NRD_SHADERMAKE_INPUTS CONFIGURE_DEPENDS
        "${_shadermake_source_dir}/*.c"
        "${_shadermake_source_dir}/*.cc"
        "${_shadermake_source_dir}/*.cpp"
        "${_shadermake_source_dir}/*.h"
        "${_shadermake_source_dir}/*.hpp"
        "${_shadermake_source_dir}/CMakeLists.txt")
endif()
if(_shadermake_binary_dir AND NOT _shadermake_binary_dir MATCHES "NOTFOUND")
    set(NRD_SHADERMAKE_REBUILD_COMMAND
        COMMAND ${CMAKE_COMMAND} --build "${_shadermake_binary_dir}" --target ShaderMake --config Release)
endif()

# file(GENERATE) updates the signature only when effective content changes. The per-configuration
# path handles Visual Studio's multi-config generator and explicitly participates in invalidation.
set(NRD_SHADER_SIGNATURE "${CMAKE_CURRENT_BINARY_DIR}/nrd-shader-signature-$<CONFIG>.txt")
string(CONCAT NRD_SHADER_SIGNATURE_CONTENT
    "config=$<CONFIG>\n"
    "platforms=${NRD_SHADER_PLATFORMS}\n"
    "shader_make=${SHADERMAKE_PATH}\n"
    "compilers=${NRD_SHADER_COMPILERS}\n"
    "arguments=${SHADERMAKE_GENERAL_ARGS}\n"
    "normal_encoding=${NRD_NORMAL_ENCODING}\n"
    "roughness_encoding=${NRD_ROUGHNESS_ENCODING}\n"
    "compile_definitions=${COMPILE_DEFINITIONS}\n")
file(GENERATE OUTPUT "${NRD_SHADER_SIGNATURE}" CONTENT "${NRD_SHADER_SIGNATURE_CONTENT}")

if(NRD_SHADER_OUTPUTS)
    add_custom_command(
        OUTPUT ${NRD_SHADER_OUTPUTS}
        ${NRD_SHADERMAKE_REBUILD_COMMAND}
        ${NRD_SHADER_COMMANDS}
        DEPENDS
            ShaderMake
            ${SHADERS}
            ${NRD_MATHLIB_SHADER_INPUTS}
            ${NRD_SHADERMAKE_INPUTS}
            ${NRD_SHADER_COMPILERS}
            "${NRD_SHADER_SIGNATURE}"
            "${CMAKE_CURRENT_LIST_FILE}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Generating incremental NRD shader containers"
        VERBATIM
        COMMAND_EXPAND_LISTS)
    add_custom_target(NRDShaders DEPENDS ${NRD_SHADER_OUTPUTS})
    set_source_files_properties(${NRD_SHADER_OUTPUTS} PROPERTIES
        GENERATED TRUE
        HEADER_FILE_ONLY TRUE)
    target_sources(NRD PRIVATE ${NRD_SHADER_OUTPUTS})
else()
    add_custom_target(NRDShaders)
endif()

set_target_properties(NRDShaders PROPERTIES FOLDER "NRD")
