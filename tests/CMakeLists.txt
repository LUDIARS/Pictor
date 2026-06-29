# Pictor test suite.
#
# Tests are headless (no GLFW / Vulkan window required) and rely only on
# the static `pictor` library. Each test is its own executable that exits
# 0 on pass and non-zero on failure, so CTest reports it directly.
#
# Layers:
#   Layer 1 (unit): radix sort, frame allocator, frustum culling
#   Layer 2 (integration): pixel analysis (CPU rasterizer), FPS baseline

set(PICTOR_TESTS
    unit_frame_allocator_test
    unit_radix_sort_test
    unit_culling_test
    unit_visus_serializer_test
    unit_visus_instantiator_test
    unit_shader_registry_test
    unit_pipeline_profile_serializer_test
    unit_postprocess_chain_test
    unit_attachment_def_test
    unit_pipeline_registries_test
    unit_parser_dos_test
    unit_gpu_ring_flight_test
    unit_gpu_timer_test
    unit_perf_introspection_test
    unit_cache_info_test
    unit_device_memory_profile_test
    pixel_text_test
    fps_baseline_test
)

foreach(_t ${PICTOR_TESTS})
    add_executable(${_t} ${_t}.cpp)
    target_link_libraries(${_t} PRIVATE pictor)
    target_include_directories(${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # MSVC: tests も UTF-8 ソースとして読む (日本語コメントを含む header を取り込むため)
    if(MSVC)
        target_compile_options(${_t} PRIVATE /utf-8)
    endif()
    add_test(NAME ${_t}
             COMMAND ${_t}
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
endforeach()

# Pixel test needs the bundled font next to the working directory.
if(TARGET pictor_fonts)
    foreach(_t pixel_text_test)
        add_dependencies(${_t} pictor_fonts)
    endforeach()
endif()

# Reasonable per-test timeout — units finish in milliseconds, FPS test
# completes in well under 5 seconds on any modern host.
set_tests_properties(${PICTOR_TESTS} PROPERTIES TIMEOUT 60)
