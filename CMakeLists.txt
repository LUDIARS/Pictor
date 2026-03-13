cmake_minimum_required(VERSION 3.20)
project(Pictor VERSION 2.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PICTOR_BUILD_BENCHMARK "Build 1M Spheres benchmark" ON)
option(PICTOR_BUILD_DEMO      "Build Vulkan window demo"   ON)
option(PICTOR_ENABLE_PROFILER "Enable built-in profiler" ON)
option(PICTOR_USE_LARGE_PAGES "Enable large page support" OFF)

# --- Vulkan (requires Vulkan SDK to be installed) ---
find_package(Vulkan QUIET)

# --- GLFW (auto-download via FetchContent if not found) ---
include(FetchContent)
find_package(glfw3 QUIET)
if(NOT glfw3_FOUND)
    message(STATUS "Pictor: glfw3 not found on system — fetching via FetchContent")
    set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG        3.4
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(glfw)
    set(glfw3_FOUND TRUE)
endif()

add_library(pictor STATIC
    # Core
    src/core/types.cpp

    # Memory Subsystem
    src/memory/frame_allocator.cpp
    src/memory/pool_allocator.cpp
    src/memory/gpu_memory_allocator.cpp
    src/memory/memory_subsystem.cpp

    # Scene Registry
    src/scene/soa_stream.cpp
    src/scene/object_pool.cpp
    src/scene/object_classifier.cpp
    src/scene/scene_registry.cpp

    # Data Update
    src/update/job_dispatcher.cpp
    src/update/update_scheduler.cpp

    # Batching + Sort
    src/batch/radix_sort.cpp
    src/batch/batch_builder.cpp

    # Culling
    src/culling/flat_bvh.cpp
    src/culling/frustum_culler.cpp
    src/culling/culling_system.cpp

    # GPU Driven
    src/gpu/gpu_buffer_manager.cpp
    src/gpu/gpu_driven_pipeline.cpp

    # Global Illumination
    src/gi/gi_lighting_system.cpp
    src/gi/gi_bake.cpp

    # Material
    src/material/base_material_builder.cpp

    # Pipeline
    src/pipeline/pipeline_profile.cpp
    src/pipeline/render_pass_scheduler.cpp
    src/pipeline/command_encoder.cpp

    # Profiler
    src/profiler/cpu_timer.cpp
    src/profiler/gpu_timer.cpp
    src/profiler/profiler.cpp
    src/profiler/overlay_renderer.cpp
    src/profiler/data_exporter.cpp

    # Surface Abstraction
    src/surface/vulkan_context.cpp
    src/surface/glfw_surface_provider.cpp

    # Data Handler
    src/data/texture_registry.cpp
    src/data/vertex_data_uploader.cpp
    src/data/data_handler.cpp
    src/data/data_query_api.cpp

    # Public API
    src/core/pictor_renderer.cpp
)

target_include_directories(pictor PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(Vulkan_FOUND)
    target_link_libraries(pictor PUBLIC Vulkan::Vulkan)
    target_compile_definitions(pictor PUBLIC PICTOR_HAS_VULKAN=1)
endif()

if(PICTOR_ENABLE_PROFILER)
    target_compile_definitions(pictor PUBLIC PICTOR_PROFILER_ENABLED=1)
endif()

if(PICTOR_USE_LARGE_PAGES)
    target_compile_definitions(pictor PRIVATE PICTOR_LARGE_PAGES=1)
endif()

# Platform-specific flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pictor PRIVATE -Wall -Wextra -mavx2)
elseif(MSVC)
    target_compile_options(pictor PRIVATE /W4 /arch:AVX2)
endif()

if(glfw3_FOUND)
    target_link_libraries(pictor PUBLIC glfw)
endif()

if(PICTOR_BUILD_BENCHMARK)
    add_executable(pictor_benchmark benchmark/main.cpp)
    target_link_libraries(pictor_benchmark PRIVATE pictor)
endif()

if(PICTOR_BUILD_DEMO AND Vulkan_FOUND AND glfw3_FOUND)
    add_executable(pictor_demo demo/main.cpp)
    target_link_libraries(pictor_demo PRIVATE pictor)
    message(STATUS "Pictor: Vulkan window demo enabled")
elseif(PICTOR_BUILD_DEMO)
    message(STATUS "Pictor: Demo disabled (requires Vulkan + GLFW3)")
endif()
