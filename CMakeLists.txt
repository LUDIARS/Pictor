cmake_minimum_required(VERSION 3.20)
project(Pictor VERSION 2.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PICTOR_BUILD_DEMO          "Build all demo applications"      ON)
option(PICTOR_ENABLE_PROFILER     "Enable built-in profiler"        ON)
option(PICTOR_USE_LARGE_PAGES     "Enable large page support"       OFF)

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
    src/profiler/stats_overlay.cpp
    src/profiler/data_exporter.cpp
    src/profiler/bitmap_text_renderer.cpp

    # Surface Abstraction
    src/surface/vulkan_context.cpp
    src/surface/glfw_surface_provider.cpp
    src/surface/simple_renderer.cpp

    # Data Handler
    src/data/texture_registry.cpp
    src/data/vertex_data_uploader.cpp
    src/data/data_handler.cpp
    src/data/data_query_api.cpp

    # Text Rendering
    src/text/font_loader.cpp
    src/text/text_image_renderer.cpp
    src/text/text_svg_renderer.cpp
    src/text/text_rasterizer.cpp

    # Animation System
    src/animation/animation_clip.cpp
    src/animation/skeleton.cpp
    src/animation/ik_solver.cpp
    src/animation/motion_estimator.cpp
    src/animation/fbx_importer.cpp
    src/animation/bvh_importer.cpp
    src/animation/animation_2d.cpp
    src/animation/rive_animation.cpp
    src/animation/lottie_animation.cpp
    src/animation/vector_animation.cpp
    src/animation/animation_system.cpp

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
    if(WIN32)
        target_compile_definitions(pictor PUBLIC VK_USE_PLATFORM_WIN32_KHR=1 NOMINMAX)
    elseif(UNIX AND NOT APPLE)
        target_compile_definitions(pictor PUBLIC VK_USE_PLATFORM_XLIB_KHR=1)
    elseif(APPLE)
        target_compile_definitions(pictor PUBLIC VK_USE_PLATFORM_MACOS_MVK=1)
    endif()
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

if(PICTOR_BUILD_DEMO)
    # Benchmark (headless capable, no Vulkan/GLFW required)
    add_executable(pictor_benchmark demo/benchmark/main.cpp)
    target_link_libraries(pictor_benchmark PRIVATE pictor)

    if(Vulkan_FOUND AND glfw3_FOUND)
        # Vulkan window demo
        add_executable(pictor_demo demo/main.cpp)
        target_link_libraries(pictor_demo PRIVATE pictor)

        # PBR graphics demo
        add_executable(pictor_graphics_demo demo/graphics/main.cpp)
        target_link_libraries(pictor_graphics_demo PRIVATE pictor)

        # Ocean wave tessellation demo
        add_executable(pictor_ocean_demo demo/ocean/main.cpp)
        target_link_libraries(pictor_ocean_demo PRIVATE pictor)

        # Text rendering demo
        add_executable(pictor_text_demo demo/text/main.cpp)
        target_link_libraries(pictor_text_demo PRIVATE pictor)

        message(STATUS "Pictor: All demos enabled (benchmark, window, graphics, ocean, text)")
    else()
        message(STATUS "Pictor: Only benchmark demo enabled (Vulkan + GLFW3 required for other demos)")
    endif()
endif()

# Shader compilation (demo shaders only)
if(PICTOR_BUILD_DEMO AND Vulkan_FOUND AND Vulkan_GLSLC_EXECUTABLE)
    set(PICTOR_SHADER_SOURCES
        ${CMAKE_SOURCE_DIR}/demo/shaders/simple_inst.vert
        ${CMAKE_SOURCE_DIR}/demo/shaders/simple_inst.frag
        ${CMAKE_SOURCE_DIR}/demo/graphics/shaders/pbr_demo.vert
        ${CMAKE_SOURCE_DIR}/demo/graphics/shaders/pbr_demo.frag
        ${CMAKE_SOURCE_DIR}/demo/ocean/shaders/ocean.vert
        ${CMAKE_SOURCE_DIR}/demo/ocean/shaders/ocean.tesc
        ${CMAKE_SOURCE_DIR}/demo/ocean/shaders/ocean.tese
        ${CMAKE_SOURCE_DIR}/demo/ocean/shaders/ocean.frag
        ${CMAKE_SOURCE_DIR}/demo/ocean/shaders/ocean_wireframe.frag
        ${CMAKE_SOURCE_DIR}/demo/text/shaders/text_quad.vert
        ${CMAKE_SOURCE_DIR}/demo/text/shaders/text_quad.frag
        ${CMAKE_SOURCE_DIR}/demo/shaders/text_overlay.vert
        ${CMAKE_SOURCE_DIR}/demo/shaders/text_overlay.frag
    )
    set(PICTOR_SPIRV_FILES)
    foreach(SHADER ${PICTOR_SHADER_SOURCES})
        get_filename_component(SHADER_NAME ${SHADER} NAME)
        set(SPIRV_OUT ${CMAKE_BINARY_DIR}/shaders/${SHADER_NAME}.spv)
        add_custom_command(
            OUTPUT ${SPIRV_OUT}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
            COMMAND ${Vulkan_GLSLC_EXECUTABLE} ${SHADER} -o ${SPIRV_OUT}
            DEPENDS ${SHADER}
            COMMENT "Compiling ${SHADER_NAME} -> SPIR-V"
        )
        list(APPEND PICTOR_SPIRV_FILES ${SPIRV_OUT})
    endforeach()
    add_custom_target(pictor_shaders ALL DEPENDS ${PICTOR_SPIRV_FILES})
    if(TARGET pictor_demo)
        add_dependencies(pictor_demo pictor_shaders)
    endif()
    if(TARGET pictor_benchmark)
        add_dependencies(pictor_benchmark pictor_shaders)
    endif()
    if(TARGET pictor_graphics_demo)
        add_dependencies(pictor_graphics_demo pictor_shaders)
    endif()
    if(TARGET pictor_ocean_demo)
        add_dependencies(pictor_ocean_demo pictor_shaders)
    endif()
    if(TARGET pictor_text_demo)
        add_dependencies(pictor_text_demo pictor_shaders)
    endif()
endif()
