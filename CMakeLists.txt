cmake_minimum_required(VERSION 3.20)
project(Pictor VERSION 2.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PICTOR_BUILD_DEMO          "Build all demo applications"      ON)
option(PICTOR_BUILD_TOOLS         "Build developer tool targets (feature selector etc.)" OFF)
option(PICTOR_BUILD_TESTS         "Build the headless test suite (CTest)"   ON)
option(PICTOR_ENABLE_PROFILER     "Enable built-in profiler"        ON)
option(PICTOR_USE_LARGE_PAGES     "Enable large page support"       OFF)
option(PICTOR_BUILD_WEBGL         "Build WebGL2 backend (Emscripten)" OFF)
option(PICTOR_BUILD_C_API         "Build C ABI export library (for WASM / Capacitor / N-API consumers)" OFF)
option(PICTOR_ENABLE_RIVE         "Enable Rive Renderer integration (requires prebuilt rive-runtime; see cmake/FindRive.cmake)" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

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
    src/culling/world_partition.cpp

    # GPU Driven
    src/gpu/gpu_buffer_manager.cpp
    src/gpu/gpu_driven_pipeline.cpp

    # Global Illumination
    src/gi/gi_lighting_system.cpp
    src/gi/gi_bake.cpp

    # Post-Process
    src/postprocess/bloom_effect.cpp
    src/postprocess/depth_of_field_effect.cpp
    src/postprocess/tone_mapping_effect.cpp
    src/postprocess/gaussian_blur_effect.cpp
    src/postprocess/postprocess_pipeline.cpp

    # Material
    src/material/base_material_builder.cpp
    src/material/material_serializer.cpp

    # Pipeline
    src/pipeline/pipeline_profile.cpp
    src/pipeline/pipeline_builder.cpp
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
    src/data/model_data_handler.cpp
    src/data/data_handler.cpp
    src/data/data_query_api.cpp

    # Text Rendering
    src/text/font_loader.cpp
    src/text/text_image_renderer.cpp
    src/text/text_svg_renderer.cpp
    src/text/text_rasterizer.cpp
    src/text/text_effects.cpp
    src/text/glyph_path_effects.cpp

    # Animation System
    src/animation/animation_clip.cpp
    src/animation/skeleton.cpp
    src/animation/ik_solver.cpp
    src/animation/motion_estimator.cpp
    src/animation/fbx_importer.cpp
    src/animation/fbx_document.cpp
    src/animation/fbx_scene.cpp
    src/animation/bvh_importer.cpp
    src/animation/animation_2d.cpp
    src/animation/rive_animation.cpp
    src/animation/lottie_animation.cpp
    src/animation/vector_animation.cpp
    src/animation/animation_system.cpp

    # Vector (Rive Renderer integration)
    src/vector/rive_renderer.cpp

    # UI Overlay
    src/ui/screen_overlay_group.cpp
    src/ui/ui_overlay_pipeline.cpp

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

# ─── Rive Renderer (optional) ────────────────────────────────
# Rive's own build is premake5-based; we consume it as a prebuilt static
# library. User workflow:
#   git clone https://github.com/rive-app/rive-runtime
#   cd rive-runtime && ./build.sh release
#   cmake -DPICTOR_ENABLE_RIVE=ON -DPICTOR_RIVE_DIR=/path/to/rive-runtime ...
if(PICTOR_ENABLE_RIVE)
    find_package(Rive REQUIRED)
    target_link_libraries(pictor PUBLIC Rive::Rive)
    target_compile_definitions(pictor PUBLIC PICTOR_HAS_RIVE=1)
    message(STATUS "Pictor: Rive Renderer enabled (dir=${PICTOR_RIVE_DIR}, config=${PICTOR_RIVE_CONFIG})")
endif()

# Platform-specific flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pictor PRIVATE -Wall -Wextra -mavx2)
elseif(MSVC)
    target_compile_options(pictor PRIVATE /W4 /arch:AVX2 /utf-8)
endif()

if(glfw3_FOUND)
    target_link_libraries(pictor PUBLIC glfw)
endif()

if(PICTOR_BUILD_DEMO)
    # Benchmark (headless capable, no Vulkan/GLFW required)
    add_executable(pictor_benchmark demo/benchmark/main.cpp)
    target_link_libraries(pictor_benchmark PRIVATE pictor)

    # FBX loading + animation demo (headless, console output)
    add_executable(pictor_fbx_demo demo/fbx/main.cpp)
    target_link_libraries(pictor_fbx_demo PRIVATE pictor)

    # Mobile rendering pipeline demo (headless — no Vulkan/GLFW required).
    # Showcases MobileLow / MobileHigh profiles and the external
    # PipelineProfileBuilder API, so it builds on CI and on mobile toolchains
    # where a Vulkan SDK isn't available.
    add_executable(pictor_mobile_demo demo/mobile/main.cpp)
    target_link_libraries(pictor_mobile_demo PRIVATE pictor)

    # Text effects smoke test (headless). Exercises every text_effects
    # and glyph_path_effects entry point on a synthetic alpha/path pair
    # and reports pass/fail via exit code — handy for CI.
    add_executable(pictor_text_effects_demo demo/text_effects/main.cpp)
    target_link_libraries(pictor_text_effects_demo PRIVATE pictor)

    # Material JSON serializer round-trip smoke test (headless).
    # Constructs a non-default MaterialDesc, emits canonical JSON, parses
    # back, asserts field-for-field equality. Non-zero exit on divergence.
    add_executable(pictor_material_serializer_demo demo/material_serializer/main.cpp)
    target_link_libraries(pictor_material_serializer_demo PRIVATE pictor)

    # Copy sample kv-config file next to the binary so --config= works out of
    # the box from the build tree.
    add_custom_command(TARGET pictor_mobile_demo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/profiles
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/demo/mobile/profiles/mobile_custom.ini
            ${CMAKE_BINARY_DIR}/profiles/mobile_custom.ini
        COMMENT "Staging mobile demo profile config"
    )

    # FBX viewer demo (Vulkan + skinning, textured lit shader)
    if(Vulkan_FOUND AND glfw3_FOUND)
        add_executable(pictor_fbx_viewer
            demo/fbx_viewer/main.cpp
            third_party/stb/stb_image_impl.cpp
        )
        target_include_directories(pictor_fbx_viewer PRIVATE third_party/stb)
        target_link_libraries(pictor_fbx_viewer PRIVATE pictor)
    endif()

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

        # Post-process demo
        add_executable(pictor_postprocess_demo demo/postprocess/main.cpp)
        target_link_libraries(pictor_postprocess_demo PRIVATE pictor)

        # Rive Renderer demo (plays an official .riv file through pictor::RiveRenderer).
        # Always builds so Pictor's CI catches compile-time regressions in the
        # demo — when PICTOR_ENABLE_RIVE is OFF, `rive.initialize()` returns
        # false and the demo exits cleanly at startup.
        add_executable(pictor_rive_demo demo/rive/main.cpp)
        target_link_libraries(pictor_rive_demo PRIVATE pictor)

        # Rive Cube demo: 6 Rive samples rendered onto cube faces.
        add_executable(pictor_rive_cube_demo demo/rive_cube/main.cpp)
        target_link_libraries(pictor_rive_cube_demo PRIVATE pictor)

        # 2D Texture rendering demo
        add_executable(pictor_texture2d_demo
            demo/texture2d/main.cpp
            demo/texture2d/texture2d_renderer.cpp
            third_party/stb/stb_image_impl.cpp
        )
        target_include_directories(pictor_texture2d_demo PRIVATE demo/texture2d third_party/stb)
        target_link_libraries(pictor_texture2d_demo PRIVATE pictor)

        message(STATUS "Pictor: All demos enabled (benchmark, window, graphics, ocean, text, postprocess, texture2d)")
    else()
        message(STATUS "Pictor: Only benchmark demo enabled (Vulkan + GLFW3 required for other demos)")
    endif()
endif()

# ============================================================
# WebGL2 backend (Emscripten)
# ============================================================
if(PICTOR_BUILD_WEBGL OR EMSCRIPTEN)
    add_library(pictor_webgl STATIC
        src/core/types.cpp
        src/webgl/webgl_context.cpp
        src/webgl/webgl_shader.cpp
        src/webgl/webgl_buffer.cpp
        src/webgl/webgl_renderer.cpp
    )

    target_include_directories(pictor_webgl PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )

    target_compile_definitions(pictor_webgl PUBLIC PICTOR_HAS_WEBGL=1)

    if(EMSCRIPTEN)
        target_compile_options(pictor_webgl PRIVATE -sUSE_WEBGL2=1)
        target_link_options(pictor_webgl PUBLIC -sUSE_WEBGL2=1 -sFULL_ES3=1)
    endif()

    # WebGL demo (Emscripten only)
    if(PICTOR_BUILD_DEMO AND EMSCRIPTEN)
        add_executable(pictor_webgl_demo demo/webgl/main.cpp)
        target_link_libraries(pictor_webgl_demo PRIVATE pictor_webgl)
        target_link_options(pictor_webgl_demo PRIVATE
            -sUSE_WEBGL2=1
            -sFULL_ES3=1
            -sALLOW_MEMORY_GROWTH=1
            -sEXPORTED_RUNTIME_METHODS=['ccall','cwrap']
            --shell-file ${CMAKE_SOURCE_DIR}/demo/webgl/shell.html
        )
        set_target_properties(pictor_webgl_demo PROPERTIES
            SUFFIX ".html"
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/webgl
        )
        # Copy shaders
        add_custom_command(TARGET pictor_webgl_demo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_SOURCE_DIR}/shaders/webgl
                ${CMAKE_BINARY_DIR}/webgl/shaders
        )
        message(STATUS "Pictor: WebGL2 demo enabled (Emscripten)")
    endif()
endif()

# ============================================================
# C ABI export (for WASM / Capacitor / N-API consumers)
# ============================================================
if(PICTOR_BUILD_C_API)
    add_library(pictor_c_api STATIC
        src/c_api/c_api.cpp
    )
    target_include_directories(pictor_c_api PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    target_compile_features(pictor_c_api PUBLIC cxx_std_20)
    target_compile_definitions(pictor_c_api PUBLIC PICTOR_HAS_C_API=1)

    # Emscripten 環境では cwrap で叩く想定なので関数を export させる
    if(EMSCRIPTEN)
        target_link_options(pictor_c_api PUBLIC
            -sEXPORTED_FUNCTIONS=['_pictor_create_renderer_webgl','_pictor_destroy_renderer','_pictor_set_size','_pictor_begin_scene','_pictor_end_scene','_pictor_submit_object','_pictor_update_object','_pictor_remove_object','_pictor_render_frame','_pictor_create_texture_rgba8','_pictor_release_texture','_pictor_last_error','_pictor_c_api_version']
            -sEXPORTED_RUNTIME_METHODS=['ccall','cwrap']
        )
    endif()

    message(STATUS "Pictor: C ABI export enabled (target pictor_c_api)")
endif()

# Shader compilation (demo shaders only)
if(PICTOR_BUILD_DEMO AND Vulkan_FOUND AND Vulkan_GLSLC_EXECUTABLE)
    set(PICTOR_SHADER_SOURCES
        ${CMAKE_SOURCE_DIR}/demo/shaders/simple_inst.vert
        ${CMAKE_SOURCE_DIR}/demo/shaders/simple_inst.frag
        ${CMAKE_SOURCE_DIR}/shaders/lit.vert
        ${CMAKE_SOURCE_DIR}/shaders/lit.frag
        ${CMAKE_SOURCE_DIR}/shaders/hologram.vert
        ${CMAKE_SOURCE_DIR}/shaders/hologram.frag
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
        ${CMAKE_SOURCE_DIR}/shaders/postprocess/fullscreen_quad.vert
        ${CMAKE_SOURCE_DIR}/shaders/postprocess/bloom_extract.comp
        ${CMAKE_SOURCE_DIR}/shaders/postprocess/bloom_downsample.comp
        ${CMAKE_SOURCE_DIR}/shaders/postprocess/bloom_upsample.comp
        ${CMAKE_SOURCE_DIR}/shaders/postprocess/bloom_composite.frag
        ${CMAKE_SOURCE_DIR}/shaders/postprocess/tone_mapping.frag
        ${CMAKE_SOURCE_DIR}/shaders/postprocess/dof.comp
        ${CMAKE_SOURCE_DIR}/shaders/postprocess/gaussian_blur.comp
        ${CMAKE_SOURCE_DIR}/demo/postprocess/shaders/pp_demo_scene.vert
        ${CMAKE_SOURCE_DIR}/demo/postprocess/shaders/pp_demo_scene.frag
        ${CMAKE_SOURCE_DIR}/demo/texture2d/shaders/texture2d.vert
        ${CMAKE_SOURCE_DIR}/demo/texture2d/shaders/texture2d.frag
        ${CMAKE_SOURCE_DIR}/demo/fbx_viewer/shaders/model.vert
        ${CMAKE_SOURCE_DIR}/demo/fbx_viewer/shaders/model.frag
        ${CMAKE_SOURCE_DIR}/demo/fbx_viewer/shaders/bone.vert
        ${CMAKE_SOURCE_DIR}/demo/fbx_viewer/shaders/bone.frag
        ${CMAKE_SOURCE_DIR}/shaders/ui/ui_overlay.vert
        ${CMAKE_SOURCE_DIR}/shaders/ui/ui_overlay.frag
        ${CMAKE_SOURCE_DIR}/demo/rive_cube/shaders/rive_cube.vert
        ${CMAKE_SOURCE_DIR}/demo/rive_cube/shaders/rive_cube.frag
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
    if(TARGET pictor_postprocess_demo)
        add_dependencies(pictor_postprocess_demo pictor_shaders)
    endif()
    if(TARGET pictor_texture2d_demo)
        add_dependencies(pictor_texture2d_demo pictor_shaders)
    endif()
    if(TARGET pictor_fbx_viewer)
        add_dependencies(pictor_fbx_viewer pictor_shaders)
    endif()
endif()

# Copy fonts directory to build output
if(EXISTS ${CMAKE_SOURCE_DIR}/fonts)
    add_custom_target(pictor_fonts ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/fonts
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/fonts ${CMAKE_BINARY_DIR}/fonts
        COMMENT "Copying fonts to build directory"
    )
    foreach(_target pictor_demo pictor_text_demo pictor_graphics_demo pictor_postprocess_demo pictor_ocean_demo pictor_texture2d_demo)
        if(TARGET ${_target})
            add_dependencies(${_target} pictor_fonts)
        endif()
    endforeach()
endif()

# Copy texture2d demo textures to build output
if(EXISTS ${CMAKE_SOURCE_DIR}/demo/texture2d/textures)
    add_custom_target(pictor_texture2d_assets ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/textures
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/demo/texture2d/textures
            ${CMAKE_BINARY_DIR}/textures
        COMMENT "Copying texture2d demo assets to build directory"
    )
    if(TARGET pictor_texture2d_demo)
        add_dependencies(pictor_texture2d_demo pictor_texture2d_assets)
    endif()
endif()

# Copy bundled Rive sample assets (sample1..sample5.riv) next to
# `textures/` / `shaders/` / `fonts/` so the rive demo can find them.
# Files come from rive-runtime's own webgpu_player/rivs sample pack.
if(EXISTS ${CMAKE_SOURCE_DIR}/rive)
    add_custom_target(pictor_rive_assets ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/rive
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/rive
            ${CMAKE_BINARY_DIR}/rive
        COMMENT "Copying rive demo sample assets to build directory"
    )
    if(TARGET pictor_rive_demo)
        add_dependencies(pictor_rive_demo pictor_rive_assets)
    endif()
    if(TARGET pictor_rive_cube_demo)
        add_dependencies(pictor_rive_cube_demo pictor_rive_assets)
    endif()
endif()

if(TARGET pictor_rive_cube_demo AND TARGET pictor_shaders)
    add_dependencies(pictor_rive_cube_demo pictor_shaders)
endif()

# Copy fbx_viewer test assets (UnityChan under UCL — see fbx/model1/LICENSE.md).
if(EXISTS ${CMAKE_SOURCE_DIR}/fbx)
    add_custom_target(pictor_fbx_assets ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/fbx
            ${CMAKE_BINARY_DIR}/fbx
        COMMENT "Copying fbx test assets to build directory"
    )
    if(TARGET pictor_fbx_viewer)
        add_dependencies(pictor_fbx_viewer pictor_fbx_assets)
    endif()
endif()

# ─── Tests (opt-in via CTest) ────────────────────────────────────────
if(PICTOR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ─── Developer tools (opt-in) ────────────────────────────────────────
# Exposes custom targets under tools/ — purely developer conveniences,
# no compilation, no impact on libpictor size. When PICTOR_BUILD_TOOLS=ON
# the available targets are:
#
#   pictor_feature_selector  — open tools/feature-selector/index.html
#   pictor_measure_sizes     — rebuild libpictor once per toggle and
#                              write measurements.json for the selector
#
# The feature selector is a static HTML tool so it runs anywhere with a
# browser (including file://). The measure target needs Python 3 on PATH.
if(PICTOR_BUILD_TOOLS)
    set(_pictor_tools_selector "${CMAKE_SOURCE_DIR}/tools/feature-selector/index.html")
    if(EXISTS "${_pictor_tools_selector}")
        if(WIN32)
            add_custom_target(pictor_feature_selector
                COMMAND cmd /c start "" "${_pictor_tools_selector}"
                COMMENT "Opening feature-selector in the default browser")
        elseif(APPLE)
            add_custom_target(pictor_feature_selector
                COMMAND open "${_pictor_tools_selector}"
                COMMENT "Opening feature-selector in the default browser")
        else()
            add_custom_target(pictor_feature_selector
                COMMAND xdg-open "${_pictor_tools_selector}"
                COMMENT "Opening feature-selector in the default browser")
        endif()

        find_package(Python3 QUIET COMPONENTS Interpreter)
        if(Python3_Interpreter_FOUND)
            add_custom_target(pictor_measure_sizes
                COMMAND ${Python3_EXECUTABLE}
                    "${CMAKE_SOURCE_DIR}/tools/feature-selector/scripts/measure_sizes.py"
                    --src-dir   "${CMAKE_SOURCE_DIR}"
                    --build-dir "${CMAKE_BINARY_DIR}/_measure"
                    --config    "$<CONFIG>"
                    --out       "${CMAKE_SOURCE_DIR}/tools/feature-selector/measurements.json"
                COMMENT "Measuring per-feature libpictor size — this rebuilds multiple times, be patient")
        endif()

        message(STATUS "Pictor: developer tools enabled (tools/feature-selector)")
    else()
        message(WARNING "Pictor: PICTOR_BUILD_TOOLS=ON but tools/feature-selector not found")
    endif()
endif()
