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
    unit_batch_builder_test
    unit_culling_test
    unit_transform_math_test
    unit_float_parse_test
    unit_visus_metadata_test
    unit_visus_serializer_test
    unit_visus_catalog_test
    unit_visus_shader_package_test
    unit_visus_package_catalog_test
    unit_visus_runtime_test
    unit_visus_migrate_test
    unit_visus_resource_loader_test
    unit_visus_instantiator_test
    unit_shader_registry_test
    unit_gi_probe_field_test
    unit_gi_bake_test
    unit_attachment_def_test
    unit_pipeline_registries_test
    unit_compiled_graph_wiring_test
    unit_parser_dos_test
    unit_file_watch_test
    unit_pipeline_hot_reload_test
    unit_gpu_ring_flight_test
    unit_swapchain_per_image_pool_test
    unit_gpu_timer_test
    unit_frame_contract_test
    unit_surface_recovery_contract_test
    unit_animation_stub_contract_test
    unit_public_animation_header_test
    unit_fbx_track_csv_test
    unit_fbx_morph_targets_test
    unit_web_mesh_input_test
    pixel_text_test
    fps_baseline_test
)

if(PICTOR_ENABLE_POSTPROCESS)
    list(APPEND PICTOR_TESTS unit_pipeline_profile_serializer_test unit_pipeline_tiers_test unit_postprocess_chain_test)
endif()

if(PICTOR_ENABLE_XR)
    list(APPEND PICTOR_TESTS unit_xr_stereo_camera_test
         unit_xr_reprojection_test unit_xr_delta_planner_test)
endif()

if(PICTOR_ENABLE_SHARC)
    list(APPEND PICTOR_TESTS unit_sharc_hash_test unit_sharc_estimator_test)
endif()

foreach(_t ${PICTOR_TESTS})
    add_executable(${_t} ${_t}.cpp)
    target_link_libraries(${_t} PRIVATE pictor)
    if(PICTOR_ENABLE_POSTPROCESS AND _t MATCHES "^(unit_pipeline_profile_serializer_test|unit_pipeline_tiers_test|unit_postprocess_chain_test)$")
        target_link_libraries(${_t} PRIVATE Pictor::postprocess)
    endif()
    if(PICTOR_ENABLE_XR AND _t MATCHES "^unit_xr_")
        target_link_libraries(${_t} PRIVATE Pictor::xr)
    endif()
    target_include_directories(${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # MSVC: tests も UTF-8 ソースとして読む (日本語コメントを含む header を取り込むため)
    if(MSVC)
        target_compile_options(${_t} PRIVATE /utf-8)
    endif()
    add_test(NAME ${_t}
             COMMAND ${_t}
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
endforeach()

# FBX viewer track playback helpers (SPEC-PC-FBX-TRACK-PLAYBACK): the
# headless demo sources are compiled straight into their tests.
set(_fbx_viewer_dir ${CMAKE_CURRENT_SOURCE_DIR}/../demo/fbx_viewer)
target_sources(unit_fbx_track_csv_test PRIVATE ${_fbx_viewer_dir}/track_csv.cpp)
target_sources(unit_fbx_morph_targets_test PRIVATE ${_fbx_viewer_dir}/morph_targets.cpp)
foreach(_t unit_fbx_track_csv_test unit_fbx_morph_targets_test)
    target_include_directories(${_t} PRIVATE ${_fbx_viewer_dir})
endforeach()

# Web mesh module (SPEC-PC-WEB-MESH): the input contract is pure CPU code, so it is compiled
# straight into a native test even though the module itself only ships in WebGL builds.
target_sources(unit_web_mesh_input_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/webgl/web_mesh_input.cpp)

# The JavaScript face of the web mesh module is tested with Node against a fake wasm module.
find_program(PICTOR_NODE_EXECUTABLE node)
if(PICTOR_NODE_EXECUTABLE)
    add_test(NAME web_mesh_js_test
             COMMAND ${PICTOR_NODE_EXECUTABLE} --test "test/*.test.mjs"
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../web-mesh)
    set_tests_properties(web_mesh_js_test PROPERTIES TIMEOUT 60)
else()
    message(STATUS "Pictor: node not found — web_mesh_js_test is not registered")
endif()

# Pixel test needs the bundled font next to the working directory.
if(TARGET pictor_fonts)
    foreach(_t pixel_text_test)
        add_dependencies(${_t} pictor_fonts)
    endforeach()
endif()

# Reasonable per-test timeout — units finish in milliseconds, FPS test
# completes in well under 5 seconds on any modern host.
set_tests_properties(${PICTOR_TESTS} PROPERTIES TIMEOUT 60)
