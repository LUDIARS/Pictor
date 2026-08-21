cmake_minimum_required(VERSION 3.20)
project(Pictor VERSION 2.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PICTOR_BUILD_DEMO          "Build all demo applications"      ON)
option(PICTOR_BUILD_TOOLS         "Build developer tool targets (feature selector etc.)" OFF)
option(PICTOR_BUILD_TESTS         "Build the headless test suite (CTest)"   ON)
option(PICTOR_ENABLE_PROFILER     "Enable built-in profiler"        ON)
option(PICTOR_USE_LARGE_PAGES     "Enable large page support"       OFF)
option(PICTOR_BUILD_WEBGL         "Build WebGL2 backend (Emscripten)" OFF)
option(PICTOR_ENABLE_RIVE         "Enable Rive Renderer integration (requires prebuilt rive-runtime; see cmake/FindRive.cmake)" OFF)
option(PICTOR_ENABLE_HW_COUNTERS  "Enable Intel PCM hardware counters (perf C 層, requires PICTOR_PCM_DIR)" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# モバイル (Android NDK / iOS) クロスビルドの早期分岐 — Issue #47
#
#   Android: NDK 同梱 Vulkan を直接指す。 GLFW は Android ターゲットを持たない
#            ので FetchContent を止める。
#   iOS    : MoltenVK 経由で Vulkan。 同じく GLFW は使わない。
# どちらも `find_package(Vulkan)` / GLFW FetchContent をスキップし、
# PICTOR_HAS_VULKAN を明示的に立てる。 デスクトップ (WIN32/UNIX/APPLE-macOS)
# は従来パスのまま影響を受けない。
# ---------------------------------------------------------------------------
set(PICTOR_MOBILE OFF)
if(ANDROID)
    set(PICTOR_MOBILE ON)
    set(PICTOR_PLATFORM_ANDROID ON)
    # NDK 同梱 Vulkan (libvulkan.so + ヘッダ)。 find_package では拾えない。
    set(Vulkan_FOUND TRUE)
    set(Vulkan_LIBRARIES vulkan)
    message(STATUS "Pictor: Android NDK build (ABI=${ANDROID_ABI}, API=${ANDROID_PLATFORM})")
elseif(IOS OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(PICTOR_MOBILE ON)
    set(PICTOR_PLATFORM_IOS ON)
    # iOS は MoltenVK (Vulkan→Metal)。 ホストが MoltenVK.xcframework をリンクし、
    # -DVulkan_LIBRARIES / -DVulkan_INCLUDE_DIRS で指す想定。 ここでは
    # PICTOR_HAS_VULKAN を立てるだけ (実リンクはホスト xcodeproj 側)。
    set(Vulkan_FOUND TRUE)
    message(STATUS "Pictor: iOS build (MoltenVK 経由)")
endif()

# --- Vulkan (requires Vulkan SDK to be installed) — デスクトップのみ ---
if(NOT PICTOR_MOBILE)
    find_package(Vulkan QUIET)
endif()

# --- GLFW (auto-download via FetchContent if not found) — デスクトップのみ ---
include(FetchContent)
if(NOT PICTOR_MOBILE)
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
else()
    set(glfw3_FOUND FALSE)
endif()

add_library(pictor STATIC
    # Core
    src/core/types.cpp
    src/core/cache_info.cpp
    src/core/device_memory_profile.cpp

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
    src/gi/gi_scene_proxy.cpp
    src/gi/gi_probe_field.cpp
    src/gi/gi_gpu_executor.cpp
    src/gi/sharc_executor.cpp
    src/gi/gi_shadow_atlas.cpp
    src/gi/gi_ssao_compute.cpp
    src/gi/gi_reflection_probe.cpp

    # Post-Process (host-driven, real Vulkan)
    src/postprocess/postprocess_pipeline.cpp
    src/postprocess/postprocess_config_bridge.cpp
    src/postprocess/postprocess_chain.cpp

    # Decals (projected, host-driven)
    src/decal/decal_system.cpp

    # Material
    src/material/base_material_builder.cpp
    src/material/material_serializer.cpp

    # Shader (custom shader registry — Visus CUSTOM kind)
    src/shader/shader_registry.cpp

    # Visus (render-object definition layer)
    src/visus/visus_metadata.cpp
    src/visus/visus_types.cpp
    src/visus/visus_json.cpp
    src/visus/visus_serializer.cpp
    src/visus/visus_v1_compat.cpp
    src/visus/visus_catalog.cpp
    src/visus/visus_runtime.cpp
    src/visus/visus_instantiator.cpp
    src/visus/visus_migrate.cpp
    src/visus/resource_loader.cpp

    # Pipeline
    src/pipeline/pipeline_profile.cpp
    src/pipeline/pipeline_builder.cpp
    src/pipeline/pipeline_profile_serializer.cpp
    src/pipeline/pipeline_profile_loader.cpp
    src/pipeline/render_pass_scheduler.cpp
    src/pipeline/attachment_def.cpp
    src/pipeline/attachment_registry.cpp
    src/pipeline/render_pass_registry.cpp
    src/pipeline/framebuffer_registry.cpp
    src/pipeline/pipeline_compiler.cpp
    src/pipeline/compiled_path_driver.cpp
    src/pipeline/compiled_batch_recorder.cpp

    # Profiler
    src/profiler/cpu_timer.cpp
    src/profiler/gpu_timer.cpp
    src/profiler/profiler.cpp
    src/profiler/overlay_renderer.cpp
    src/profiler/stats_overlay.cpp
    src/profiler/data_exporter.cpp
    src/profiler/bitmap_text_renderer.cpp

    # Perf Introspection (per-object/batch 性能 & DoD キャッシュ可視化)
    src/profiler/perf_query_api.cpp
    src/profiler/batch_gpu_timer.cpp
    src/profiler/hardware_counters.cpp

    # Surface Abstraction
    # (glfw / android / ios の各プロバイダはプラットフォーム別に
    #  add_library 直後の target_sources で条件付き追加する)
    src/surface/vulkan_context.cpp
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
    src/animation/montage.cpp

    # Vector (Rive Renderer integration)
    src/vector/rive_renderer.cpp

    # UI Overlay
    src/ui/screen_overlay_group.cpp
    src/ui/ui_overlay_pipeline.cpp
    src/ui/ui_renderer.cpp

    # Public API
    src/c_api/c_api.cpp
    src/core/pictor_renderer.cpp
    src/core/renderer_subsystem_manager.cpp
    src/core/mobile_lifecycle_controller.cpp
    src/core/gi_facade.cpp
)

# サーフェスプロバイダ — プラットフォーム別に条件付きで追加。
# デスクトップ: GLFW プロバイダ。 Android/iOS: 各ネイティブプロバイダ。
if(PICTOR_PLATFORM_ANDROID)
    target_sources(pictor PRIVATE src/surface/android_surface_provider.cpp)
elseif(PICTOR_PLATFORM_IOS)
    target_sources(pictor PRIVATE src/surface/ios_surface_provider.cpp)
else()
    target_sources(pictor PRIVATE src/surface/glfw_surface_provider.cpp)
endif()

target_include_directories(pictor PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(Vulkan_FOUND)
    target_compile_definitions(pictor PUBLIC PICTOR_HAS_VULKAN=1)
    if(PICTOR_PLATFORM_ANDROID)
        # NDK 同梱の libvulkan / android / log を直接リンク。
        target_link_libraries(pictor PUBLIC vulkan android log)
        target_compile_definitions(pictor PUBLIC VK_USE_PLATFORM_ANDROID_KHR=1)
    elseif(PICTOR_PLATFORM_IOS)
        # iOS は MoltenVK。 リンクはホスト xcodeproj 側 (MoltenVK.xcframework)。
        if(Vulkan_LIBRARIES)
            target_link_libraries(pictor PUBLIC ${Vulkan_LIBRARIES})
        endif()
        if(Vulkan_INCLUDE_DIRS)
            target_include_directories(pictor PUBLIC ${Vulkan_INCLUDE_DIRS})
        endif()
        target_compile_definitions(pictor PUBLIC VK_USE_PLATFORM_METAL_EXT=1)
    else()
        target_link_libraries(pictor PUBLIC Vulkan::Vulkan)
        if(WIN32)
            target_compile_definitions(pictor PUBLIC VK_USE_PLATFORM_WIN32_KHR=1 NOMINMAX)
        elseif(UNIX AND NOT APPLE)
            target_compile_definitions(pictor PUBLIC VK_USE_PLATFORM_XLIB_KHR=1)
        elseif(APPLE)
            target_compile_definitions(pictor PUBLIC VK_USE_PLATFORM_MACOS_MVK=1)
        endif()
    endif()
endif()

if(PICTOR_ENABLE_PROFILER)
    target_compile_definitions(pictor PUBLIC PICTOR_PROFILER_ENABLED=1)
endif()

if(PICTOR_USE_LARGE_PAGES)
    target_compile_definitions(pictor PRIVATE PICTOR_LARGE_PAGES=1)
endif()

# ─── Hardware perf counters (perf C 層, optional) ─────────────
# Intel PCM をプロセス内から呼んで L2/L3 ミス・DRAM 帯域・IPC を読む。
#   git clone https://github.com/intel/pcm
#   cmake -DPICTOR_ENABLE_HW_COUNTERS=ON -DPICTOR_PCM_DIR=/path/to/pcm ...
# 要求されたのに PCM が見つからない場合は黙って無効化せず FATAL_ERROR で落とす
# (無言フォールバック禁止)。OFF のときは NullHardwareCounterProvider が理由付きで
# available=false を返す。
if(PICTOR_ENABLE_HW_COUNTERS)
    target_compile_definitions(pictor PUBLIC PICTOR_ENABLE_HW_COUNTERS=1)
    if(NOT DEFINED PICTOR_PCM_DIR)
        message(FATAL_ERROR "PICTOR_ENABLE_HW_COUNTERS=ON には PICTOR_PCM_DIR (Intel PCM のパス) が必要")
    endif()
    target_include_directories(pictor PRIVATE ${PICTOR_PCM_DIR}/src ${PICTOR_PCM_DIR})
    find_library(PCM_LIB
        NAMES pcm PCM libpcm
        HINTS ${PICTOR_PCM_DIR} ${PICTOR_PCM_DIR}/build/lib ${PICTOR_PCM_DIR}/lib)
    if(NOT PCM_LIB)
        message(FATAL_ERROR "PICTOR_ENABLE_HW_COUNTERS=ON だが PCM ライブラリが見つからない (PICTOR_PCM_DIR=${PICTOR_PCM_DIR})")
    endif()
    target_link_libraries(pictor PUBLIC ${PCM_LIB})
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
# AVX2 は x86/x64 専用命令。 ARM64 (Android arm64-v8a / iOS) では clang が
# `-mavx2` でエラーになるため、 x86 系のみに限定する。 AVX2 を使うコード
# (update_scheduler.cpp) は `#ifdef __AVX2__` でスカラ fallback 済み。
set(PICTOR_IS_X86 FALSE)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86|x64|X86|X64|AMD64|amd64|i[3-6]86")
    set(PICTOR_IS_X86 TRUE)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pictor PRIVATE -Wall -Wextra)
    if(PICTOR_IS_X86 AND NOT PICTOR_MOBILE)
        target_compile_options(pictor PRIVATE -mavx2)
    endif()
elseif(MSVC)
    target_compile_options(pictor PRIVATE /W4 /utf-8)
    if(PICTOR_IS_X86)
        target_compile_options(pictor PRIVATE /arch:AVX2)
    endif()
endif()

if(glfw3_FOUND)
    target_link_libraries(pictor PUBLIC glfw)
endif()

# デモ群は GLFW / Vulkan SDK 前提。 モバイル (Android/iOS) ではビルドしない
# (Phase 2 以降で headless demo を .so 化する場合は別途ゲートする)。
if(PICTOR_BUILD_DEMO AND PICTOR_MOBILE)
    message(STATUS "Pictor: モバイルビルドのためデモ群をスキップ (PICTOR_BUILD_DEMO=OFF 相当)")
endif()
if(PICTOR_BUILD_DEMO AND NOT PICTOR_MOBILE)
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

        # 2D Texture rendering demo
        add_executable(pictor_texture2d_demo
            demo/texture2d/main.cpp
            demo/texture2d/texture2d_renderer.cpp
            third_party/stb/stb_image_impl.cpp
        )
        target_include_directories(pictor_texture2d_demo PRIVATE demo/texture2d third_party/stb)
        target_link_libraries(pictor_texture2d_demo PRIVATE pictor)

        # SHaRC 拡張デモ (D1 roughness ladder / D2 逆光透過、 4 パス compute)
        add_executable(pictor_sharc_demo
            demo/sharc/main.cpp
            demo/sharc/ply_mesh.cpp
            demo/sharc/obj_mesh.cpp
            demo/sharc/mesh_bvh.cpp
            demo/sharc/present_renderer.cpp
            demo/sharc/gbuffer_renderer.cpp
            demo/sharc/bloom_pipeline.cpp
            demo/sharc/texture_atlas.cpp
            third_party/stb/stb_image_impl.cpp
        )
        target_include_directories(pictor_sharc_demo PRIVATE third_party/stb)
        target_link_libraries(pictor_sharc_demo PRIVATE pictor)

        # Iter relation-graph demo (instanced/DoD graph view + cull/hit-test + dock)
        add_executable(pictor_graph_demo
            demo/graph/main.cpp
            demo/graph/graph_store.cpp
            demo/graph/camera2d.cpp
            demo/graph/graph_generator.cpp
            demo/graph/layout_engine.cpp
            demo/graph/snippet_cache.cpp
            demo/graph/vk_util.cpp
            demo/graph/spatial_grid.cpp
            demo/graph/graph_renderer.cpp
            demo/graph/graph_view.cpp
            demo/graph/ui_rect_renderer.cpp
            demo/graph/dock_layout.cpp
            demo/graph/synthetic_data_channel.cpp
            demo/graph/clangd_data_channel.cpp
        )
        target_include_directories(pictor_graph_demo PRIVATE demo/graph)
        target_link_libraries(pictor_graph_demo PRIVATE pictor)
        if(MSVC)
            # /utf-8 does not propagate from the pictor lib to demo targets.
            target_compile_options(pictor_graph_demo PRIVATE /utf-8)
        endif()

        message(STATUS "Pictor: All demos enabled (benchmark, window, graphics, ocean, text, postprocess, texture2d, graph)")
    else()
        message(STATUS "Pictor: Only benchmark demo enabled (Vulkan + GLFW3 required for other demos)")
    endif()

    # SHaRC 拡張 D3D12 移植 headless テスト (Windows のみ、 Vulkan/GLFW 非依存)。
    # HLSL は D3DCompile (d3dcompiler_47) でランタイムコンパイルするため dxc /
    # pictor_shaders (SPIR-V) には依存しない。 Bistro OBJ を読み込み固定カメラで
    # 60 フレーム実行し、 出力を sharc_dx12_out.ppm へ書き出す。
    if(WIN32)
        add_executable(pictor_sharc_dx12_test
            demo/sharc/dx12_test_main.cpp
            demo/sharc/ply_mesh.cpp
            demo/sharc/obj_mesh.cpp
            demo/sharc/mesh_bvh.cpp
            demo/sharc/texture_atlas.cpp
            src/gi/sharc_dx12_executor.cpp
            third_party/stb/stb_image_impl.cpp
        )
        target_include_directories(pictor_sharc_dx12_test PRIVATE include third_party/stb)
        target_link_libraries(pictor_sharc_dx12_test PRIVATE d3d12 dxgi d3dcompiler)
        target_compile_definitions(pictor_sharc_dx12_test PRIVATE
            NOMINMAX=1
            PICTOR_SHARC_HLSL_DIR="${CMAKE_SOURCE_DIR}/shaders/sharc/hlsl")
        if(MSVC)
            # /utf-8 does not propagate from the pictor lib to demo targets
            # ([[feedback_msvc_utf8_test_targets]]). 公開ヘッダ/ソースは日本語
            # コメント込み UTF-8 のため必須。
            target_compile_options(pictor_sharc_dx12_test PRIVATE /utf-8)
        endif()
        message(STATUS "Pictor: pictor_sharc_dx12_test enabled (D3D12 SHaRC headless test)")
    endif()

    # MSVC: /utf-8 は pictor lib から demo target へ伝播しない
    # ([[feedback_msvc_utf8_test_targets]] と同じ理由)。 公開ヘッダは日本語
    # コメントを含む UTF-8 ソースなので、 CP932 解釈だと C4819 → 構文崩壊で
    # コンパイルエラーになる。 全 demo target に個別指定する。
    if(MSVC)
        foreach(_demo
            pictor_benchmark pictor_fbx_demo pictor_mobile_demo
            pictor_text_effects_demo pictor_material_serializer_demo
            pictor_fbx_viewer pictor_demo pictor_graphics_demo
            pictor_ocean_demo pictor_text_demo pictor_postprocess_demo
            pictor_rive_demo pictor_texture2d_demo pictor_sharc_demo)
            if(TARGET ${_demo})
                target_compile_options(${_demo} PRIVATE /utf-8)
            endif()
        endforeach()
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

# Shader compilation (demo shaders only)
if(PICTOR_BUILD_DEMO AND Vulkan_FOUND AND Vulkan_GLSLC_EXECUTABLE)
    set(PICTOR_SHADER_SOURCES
        ${CMAKE_SOURCE_DIR}/demo/shaders/simple_inst.vert
        ${CMAKE_SOURCE_DIR}/demo/shaders/simple_inst.frag
        ${CMAKE_SOURCE_DIR}/shaders/lit.vert
        ${CMAKE_SOURCE_DIR}/shaders/lit.frag
        ${CMAKE_SOURCE_DIR}/shaders/pbr.vert
        ${CMAKE_SOURCE_DIR}/shaders/pbr.frag
        ${CMAKE_SOURCE_DIR}/shaders/pbr_gi.frag
        ${CMAKE_SOURCE_DIR}/shaders/gi_probe_sample.comp
        ${CMAKE_SOURCE_DIR}/shaders/ssao_gen.comp
        ${CMAKE_SOURCE_DIR}/shaders/shadow_depth.vert
        ${CMAKE_SOURCE_DIR}/shaders/shadow_depth.frag
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
        ${CMAKE_SOURCE_DIR}/shaders/postprocess/bloom_extract.frag
        ${CMAKE_SOURCE_DIR}/shaders/postprocess/bloom_blur.frag
        ${CMAKE_SOURCE_DIR}/shaders/postprocess/color_grade.frag
        ${CMAKE_SOURCE_DIR}/shaders/postprocess/dof.frag
        ${CMAKE_SOURCE_DIR}/shaders/postprocess/ssao_apply.frag
        ${CMAKE_SOURCE_DIR}/shaders/postprocess/motion_blur.frag
        ${CMAKE_SOURCE_DIR}/shaders/postprocess/fxaa.frag
        ${CMAKE_SOURCE_DIR}/shaders/postprocess/taa.frag
        ${CMAKE_SOURCE_DIR}/shaders/postprocess/ssr.frag
        ${CMAKE_SOURCE_DIR}/shaders/postprocess/exposure_measure.frag
        ${CMAKE_SOURCE_DIR}/shaders/postprocess/exposure_apply.frag
        ${CMAKE_SOURCE_DIR}/shaders/postprocess/bloom_down.frag
        ${CMAKE_SOURCE_DIR}/shaders/postprocess/bloom_up.frag
        ${CMAKE_SOURCE_DIR}/shaders/postprocess/motion_blur_velocity.frag
        ${CMAKE_SOURCE_DIR}/shaders/postprocess/taa_velocity.frag
        ${CMAKE_SOURCE_DIR}/shaders/postprocess/lens_flare.frag
        ${CMAKE_SOURCE_DIR}/shaders/postprocess/volumetric_fog.frag
        ${CMAKE_SOURCE_DIR}/shaders/postprocess/ssgi_gather.frag
        ${CMAKE_SOURCE_DIR}/shaders/postprocess/ssgi_apply.frag
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
        ${CMAKE_SOURCE_DIR}/demo/graph/shaders/graph_node.vert
        ${CMAKE_SOURCE_DIR}/demo/graph/shaders/graph_edge.vert
        ${CMAKE_SOURCE_DIR}/demo/graph/shaders/graph_flat.frag
        ${CMAKE_SOURCE_DIR}/demo/graph/shaders/ui_rect.vert
        ${CMAKE_SOURCE_DIR}/demo/fbx_viewer/shaders/model.vert
        ${CMAKE_SOURCE_DIR}/demo/fbx_viewer/shaders/model.frag
        ${CMAKE_SOURCE_DIR}/demo/fbx_viewer/shaders/bone.vert
        ${CMAKE_SOURCE_DIR}/demo/fbx_viewer/shaders/bone.frag
        ${CMAKE_SOURCE_DIR}/shaders/ui/ui_overlay.vert
        ${CMAKE_SOURCE_DIR}/shaders/ui/ui_overlay.frag
        ${CMAKE_SOURCE_DIR}/shaders/sharc/sharc_hit.comp
        ${CMAKE_SOURCE_DIR}/shaders/sharc/sharc_march.comp
        ${CMAKE_SOURCE_DIR}/shaders/sharc/sharc_compact.comp
        ${CMAKE_SOURCE_DIR}/shaders/sharc/sharc_update.comp
        ${CMAKE_SOURCE_DIR}/shaders/sharc/sharc_resolve.comp
        ${CMAKE_SOURCE_DIR}/shaders/sharc/sharc_ao_bake.comp
        ${CMAKE_SOURCE_DIR}/shaders/sharc/sharc_gbuffer_resolve.comp
        ${CMAKE_SOURCE_DIR}/shaders/sharc/sharc_gbuffer.vert
        ${CMAKE_SOURCE_DIR}/shaders/sharc/sharc_gbuffer.frag
        ${CMAKE_SOURCE_DIR}/shaders/sharc/sharc_shadow.vert
        ${CMAKE_SOURCE_DIR}/shaders/sharc/sharc_bloom_extract.comp
        ${CMAKE_SOURCE_DIR}/shaders/sharc/sharc_bloom_down.comp
        ${CMAKE_SOURCE_DIR}/shaders/sharc/sharc_bloom_up.comp
        ${CMAKE_SOURCE_DIR}/shaders/sharc/sharc_present.vert
        ${CMAKE_SOURCE_DIR}/shaders/sharc/sharc_present.frag
    )
    # include される共通 GLSL — 変更時に consumer SPIR-V を再生成させる
    # (DEPENDS がトップレベル shader のみだと古い SPIR-V が残る)
    set(PICTOR_GLSL_INCLUDES
        ${CMAKE_SOURCE_DIR}/shaders/common.glsl
        ${CMAKE_SOURCE_DIR}/shaders/gi.glsl
        ${CMAKE_SOURCE_DIR}/shaders/shadow.glsl
        ${CMAKE_SOURCE_DIR}/shaders/velocity.glsl
        ${CMAKE_SOURCE_DIR}/shaders/sharc/sharc_common.glsl
        ${CMAKE_SOURCE_DIR}/shaders/sharc/sharc_scene.glsl
        ${CMAKE_SOURCE_DIR}/shaders/sharc/sharc_moments.glsl
        ${CMAKE_SOURCE_DIR}/shaders/sharc/sharc_lobes.glsl
    )
    set(PICTOR_SPIRV_FILES)
    foreach(SHADER ${PICTOR_SHADER_SOURCES})
        get_filename_component(SHADER_NAME ${SHADER} NAME)
        set(SPIRV_OUT ${CMAKE_BINARY_DIR}/shaders/${SHADER_NAME}.spv)
        add_custom_command(
            OUTPUT ${SPIRV_OUT}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
            COMMAND ${Vulkan_GLSLC_EXECUTABLE} ${SHADER} -o ${SPIRV_OUT}
            DEPENDS ${SHADER} ${PICTOR_GLSL_INCLUDES}
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
    if(TARGET pictor_graph_demo)
        add_dependencies(pictor_graph_demo pictor_shaders)
    endif()
    if(TARGET pictor_fbx_viewer)
        add_dependencies(pictor_fbx_viewer pictor_shaders)
    endif()
    if(TARGET pictor_sharc_demo)
        add_dependencies(pictor_sharc_demo pictor_shaders)
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
if(PICTOR_BUILD_TESTS AND NOT PICTOR_MOBILE)
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
# visus_migrate — v1 *.visus.json → v2 の headless CLI (tools/visus_migrate)。
# 本体は libpictor の visus_migrate_* なので CI は常にコンパイルする。 CLI 自体も
# Vulkan 非依存で小さいので PICTOR_BUILD_TOOLS に関わらず作る。
if(NOT PICTOR_MOBILE AND NOT PICTOR_BUILD_WEBGL)
    add_executable(visus_migrate tools/visus_migrate/main.cpp)
    target_link_libraries(visus_migrate PRIVATE pictor)
    if(MSVC)
        target_compile_options(visus_migrate PRIVATE /utf-8)
    endif()
endif()

if(PICTOR_BUILD_TOOLS AND NOT PICTOR_MOBILE)
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
