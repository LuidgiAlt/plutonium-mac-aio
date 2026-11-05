cmake_minimum_required(VERSION 3.27)

# Must be set before project() to drive per-target deployment-target flags.
# cmake/AddX86Sample.cmake passes it to the x86_64 samples as well.
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS deployment version")

project(x87sidecar
    LANGUAGES CXX
    DESCRIPTION "Apple Silicon x87 JIT translation hook for Rosetta 2"
)

# Default to a Release build when the user/IDE didn't pick one.
block()
    get_property(is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(NOT is_multi_config AND NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
        set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
            Debug Release RelWithDebInfo MinSizeRel)
    endif()
endblock()

# Project-wide language settings.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Keep assert() in optimized builds: a failed one declines the request and
# stock Rosetta translates that instruction (sidecar.cpp, __assert_rtn).
# -DNDEBUG is stripped from the per-config flags, which cmake places after
# add_compile_options.
foreach(_cfg IN ITEMS RELEASE RELWITHDEBINFO MINSIZEREL)
    string(REPLACE "-DNDEBUG" "" CMAKE_CXX_FLAGS_${_cfg} "${CMAKE_CXX_FLAGS_${_cfg}}")
    string(REPLACE "-DNDEBUG" "" CMAKE_C_FLAGS_${_cfg}   "${CMAKE_C_FLAGS_${_cfg}}")
endforeach()

# Enable LTO globally where supported, for the optimized configurations only.
include(CheckIPOSupported)
block()
    check_ipo_supported(RESULT ipo_ok OUTPUT ipo_msg)
    if(ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE        TRUE PARENT_SCOPE)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE PARENT_SCOPE)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL     TRUE PARENT_SCOPE)
    else()
        message(STATUS "LTO not supported: ${ipo_msg}")
    endif()
endblock()

# All binaries land in build/bin/ for both single- and multi-config generators.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(cfg IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${cfg}" cfg_upper)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg_upper} "${CMAKE_BINARY_DIR}/bin")
endforeach()

include(cmake/AddX86Sample.cmake)

add_subdirectory(rosetta_core)
add_subdirectory(rosetta_loader)

add_subdirectory(tests)
