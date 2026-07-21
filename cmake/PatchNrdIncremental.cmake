if(NOT DEFINED NRD_SOURCE_DIR)
    message(FATAL_ERROR "NRD_SOURCE_DIR is required")
endif()

set(_nrd_cmake "${NRD_SOURCE_DIR}/CMakeLists.txt")
if(NOT EXISTS "${_nrd_cmake}")
    message(FATAL_ERROR "NRD CMakeLists.txt not found: ${_nrd_cmake}")
endif()

file(READ "${_nrd_cmake}" _nrd_contents)
if(_nrd_contents MATCHES "\n;include\\(\"\\\$\\{CMAKE_SOURCE_DIR\\}/cmake/NRDIncrementalShaders\\.cmake")
    string(REPLACE "\n;include(" "\ninclude(" _nrd_contents "${_nrd_contents}")
    file(WRITE "${_nrd_cmake}" "${_nrd_contents}")
    message(STATUS "Repaired interrupted NRD incremental shader patch: ${_nrd_cmake}")
    return()
endif()
if(_nrd_contents MATCHES "NRDIncrementalShaders\\.cmake")
    return()
endif()

set(_start_marker "# Shaders\nfile(GLOB_RECURSE SHADERS")
set(_end_marker "add_dependencies(NRD NRDShaders)")
string(FIND "${_nrd_contents}" "${_start_marker}" _start)
string(FIND "${_nrd_contents}" "${_end_marker}" _end)
if(_start LESS 0 OR _end LESS 0 OR _end LESS _start)
    message(FATAL_ERROR
        "NRD 4.17.3 shader block was not found. Refusing to patch an unknown upstream layout: "
        "${_nrd_cmake}")
endif()

string(LENGTH "${_end_marker}" _end_marker_length)
math(EXPR _suffix_start "${_end} + ${_end_marker_length}")
string(SUBSTRING "${_nrd_contents}" 0 ${_start} _prefix)
string(SUBSTRING "${_nrd_contents}" ${_suffix_start} -1 _suffix)
string(CONCAT _replacement
    "# Shaders -- project-patched incremental generation (see cmake/NRDIncrementalShaders.cmake)\n"
    "include(\"\${CMAKE_SOURCE_DIR}/cmake/NRDIncrementalShaders.cmake\")")
string(CONCAT _patched "${_prefix}" "${_replacement}" "${_suffix}")
file(WRITE "${_nrd_cmake}" "${_patched}")
message(STATUS "Applied project NRD incremental shader patch: ${_nrd_cmake}")
