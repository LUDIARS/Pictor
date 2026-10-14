# pictor_core — the backend-independent part of Pictor (spec/architecture/multi-rhi.md §6 S0).
#
# Scene / update / batch / culling / CPU memory, the backend-neutral public face
# (include/pictor/gfx/) and the internal RHI common part. Nothing here includes a
# GPU API or windowing header, so it builds with every toolchain Pictor targets,
# including Emscripten (wasm). The full `pictor` library links it PUBLIC; existing
# consumers keep linking `pictor` unchanged.
#
# Included from the root CMakeLists.txt before the Emscripten early return.

option(PICTOR_RHI_NULL "Build the null rendering backend (records only, draws nothing; headless tests)" ON)

add_library(pictor_core STATIC
    # Core
    src/core/types.cpp
    src/core/file_watch.cpp
    src/core/cache_info.cpp
    src/core/device_memory_profile.cpp
    src/core/transform_math.cpp
    src/core/float_parse.cpp

    # Memory Subsystem
    src/memory/aligned_block.cpp
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
    src/update/single_thread_dispatcher.cpp
    src/update/update_scheduler.cpp

    # Batching + Sort
    src/batch/radix_sort.cpp
    src/batch/batch_builder.cpp

    # Culling
    src/culling/flat_bvh.cpp
    src/culling/frustum_culler.cpp
    src/culling/culling_system.cpp
    src/culling/world_partition.cpp

    # Backend-neutral public face (include/pictor/gfx/)
    src/gfx/backend_kind.cpp
    src/gfx/backend_report.cpp
    src/gfx/device_caps.cpp
    src/gfx/format.cpp
    src/gfx/pass_recorder.cpp

    # Internal RHI common part (src/rhi/common/)
    src/rhi/common/handle_allocator.cpp
)

if(PICTOR_RHI_NULL)
    target_sources(pictor_core PRIVATE
        src/rhi/null/null_resource_table.cpp
        src/rhi/null/null_pass_log.cpp
        src/rhi/null/null_device.cpp
    )
    # Backend macros stay PRIVATE: consumers only ever see PICTOR_HAS_RHI.
    target_compile_definitions(pictor_core PRIVATE PICTOR_RHI_NULL=1)
endif()

target_include_directories(pictor_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# The one rendering macro consumers may test: "the backend-neutral face exists".
# PICTOR_HAS_VULKAN etc. stay as they are until S4 (multi-rhi.md §5 ビルド).
target_compile_definitions(pictor_core PUBLIC PICTOR_HAS_RHI=1)

if(PICTOR_USE_LARGE_PAGES)
    target_compile_definitions(pictor_core PRIVATE PICTOR_LARGE_PAGES=1)
endif()

# Same warning / ISA flags the sources had while they were part of `pictor`.
set(_pictor_core_x86 FALSE)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86|x64|X86|X64|AMD64|amd64|i[3-6]86")
    set(_pictor_core_x86 TRUE)
endif()
if(EMSCRIPTEN)
    target_compile_options(pictor_core PRIVATE -Wall -Wextra)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pictor_core PRIVATE -Wall -Wextra)
    if(_pictor_core_x86 AND NOT PICTOR_MOBILE)
        target_compile_options(pictor_core PRIVATE -mavx2)
    endif()
elseif(MSVC)
    target_compile_options(pictor_core PRIVATE /W4 /utf-8)
    if(_pictor_core_x86)
        target_compile_options(pictor_core PRIVATE /arch:AVX2)
    endif()
endif()
unset(_pictor_core_x86)
