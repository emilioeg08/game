cmake_minimum_required(VERSION 3.25)

project(GalaxyEngine
    VERSION 0.1.0
    DESCRIPTION "Persistent galactic simulation engine"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Debug info embedded in object files (/Z7): no shared PDB server contention under parallel Ninja builds.
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")

option(GX_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(GX_FORCE_ASSERTS "Enable GX_ASSERT in optimized builds" OFF)
option(GX_ENABLE_PROFILING "Compile profiling zones (they can still be disabled at runtime)" ON)
option(GX_BUILD_TESTS "Build unit tests" ON)
option(GX_BUILD_BENCHMARKS "Build benchmarks" ON)
option(GX_BUILD_CLIENT "Build the graphical client (downloads SDL3 and Dear ImGui at configure time)" ON)

include(cmake/GxCompileOptions.cmake)

find_package(Threads REQUIRED)

add_subdirectory(Engine)
add_subdirectory(Simulation)
add_subdirectory(Space)
add_subdirectory(Game)
add_subdirectory(Scenarios)
if(GX_BUILD_CLIENT)
    add_subdirectory(ThirdParty)
endif()
add_subdirectory(Apps)

if(GX_BUILD_BENCHMARKS)
    add_subdirectory(Benchmarks)
endif()

if(GX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Tests)
endif()
