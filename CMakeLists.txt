cmake_minimum_required(VERSION 3.20)
project(Positron LANGUAGES C CXX)

# Windows only. The codebase relies on Win32 APIs and BlackBone manual-mapping.
if(NOT WIN32)
    message(FATAL_ERROR "Positron is Windows-only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Match the msbuild layout: build/<Platform>/<Configuration>/
if(NOT DEFINED POSITRON_PLATFORM_DIR)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(POSITRON_PLATFORM_DIR "x64")
    else()
        set(POSITRON_PLATFORM_DIR "Win32")
    endif()
endif()

# Multi-config (VS) gets per-config subdirs; single-config (Ninja) uses one.
if(CMAKE_CONFIGURATION_TYPES)
    foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
        string(TOUPPER ${cfg} CFG_UP)
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CFG_UP}
            "${CMAKE_SOURCE_DIR}/build/${POSITRON_PLATFORM_DIR}/${cfg}")
        set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CFG_UP}
            "${CMAKE_SOURCE_DIR}/build/${POSITRON_PLATFORM_DIR}/${cfg}")
        set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${CFG_UP}
            "${CMAKE_SOURCE_DIR}/build/${POSITRON_PLATFORM_DIR}/${cfg}")
    endforeach()
else()
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
    endif()
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY
        "${CMAKE_SOURCE_DIR}/build/${POSITRON_PLATFORM_DIR}/${CMAKE_BUILD_TYPE}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
endif()

# Default MSVC runtime: dynamic CRT (/MD, /MDd). payload overrides to /MT.
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# INTERFACE target carrying our preferred MSVC flags / defines. First-party
# targets link this; third-party builds (BlackBone, replxx, minhook) deliberately
# don't, since NOMINMAX / WIN32_LEAN_AND_MEAN break their windows.h usage.
add_library(positron_project_options INTERFACE)
if(MSVC)
    target_compile_options(positron_project_options INTERFACE
        /W3 /Zc:__cplusplus /permissive-)
    target_compile_definitions(positron_project_options INTERFACE
        _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
add_library(positron::project_options ALIAS positron_project_options)

# Header-only third-party libs surfaced as INTERFACE targets so consumers can
# just `target_link_libraries(... third_party::<name>)`.
add_library(positron_cli11 INTERFACE)
target_include_directories(positron_cli11 INTERFACE ${CMAKE_SOURCE_DIR}/third_party/cli11/include)
add_library(third_party::cli11 ALIAS positron_cli11)

add_library(positron_nlohmann_json INTERFACE)
target_include_directories(positron_nlohmann_json INTERFACE ${CMAKE_SOURCE_DIR}/third_party/nlohmann/single_include)
add_library(third_party::nlohmann_json ALIAS positron_nlohmann_json)

add_library(positron_doctest INTERFACE)
target_include_directories(positron_doctest INTERFACE ${CMAKE_SOURCE_DIR}/third_party/doctest)
add_library(third_party::doctest ALIAS positron_doctest)

# Compiled third-party deps (BlackBone, minhook, replxx).
add_subdirectory(third_party)

# Project libraries / binaries.
add_subdirectory(shared)
add_subdirectory(sdk)
add_subdirectory(payload)
add_subdirectory(host)

option(POSITRON_BUILD_EXAMPLES "Build the SDK example apps" ON)
option(POSITRON_BUILD_TESTS    "Build the unit and integration tests" ON)

if(POSITRON_BUILD_EXAMPLES)
    add_subdirectory(examples/eval_example)
endif()
if(POSITRON_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/unit_host)
    add_subdirectory(tests/unit_payload)
    add_subdirectory(tests/integration)
endif()
