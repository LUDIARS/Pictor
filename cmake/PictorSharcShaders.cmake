# Shader packaging for the Vulkan SHaRC module, including its presentation
# passes. This target is usable without building any demo application.
set(PICTOR_SHARC_SPIRV_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
if(NOT Vulkan_GLSLC_EXECUTABLE)
    message(STATUS "Pictor SHaRC: glslc unavailable; supply precompiled SPIR-V to initialize()")
    return()
endif()

set(_sharc_root "${CMAKE_CURRENT_SOURCE_DIR}/shaders/sharc")
set(_sharc_sources
    sharc_hit.comp sharc_march.comp sharc_compact.comp sharc_update.comp
    sharc_resolve.comp sharc_ao_bake.comp sharc_gbuffer_resolve.comp
    sharc_gbuffer.vert sharc_gbuffer.frag sharc_shadow.vert
    sharc_bloom_extract.comp sharc_bloom_down.comp sharc_bloom_up.comp
    sharc_present.vert sharc_present.frag)
set(_sharc_includes
    "${_sharc_root}/sharc_common.glsl"
    "${_sharc_root}/sharc_scene.glsl"
    "${_sharc_root}/sharc_moments.glsl"
    "${_sharc_root}/sharc_lobes.glsl")
set(_sharc_outputs)
foreach(_shader IN LISTS _sharc_sources)
    set(_output "${PICTOR_SHARC_SPIRV_DIR}/${_shader}.spv")
    add_custom_command(
        OUTPUT "${_output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PICTOR_SHARC_SPIRV_DIR}"
        COMMAND "${Vulkan_GLSLC_EXECUTABLE}" "${_sharc_root}/${_shader}" -o "${_output}"
        DEPENDS "${_sharc_root}/${_shader}" ${_sharc_includes}
        COMMENT "Compiling SHaRC ${_shader} -> SPIR-V"
        VERBATIM)
    list(APPEND _sharc_outputs "${_output}")
endforeach()
add_custom_target(pictor_sharc_shaders DEPENDS ${_sharc_outputs})
add_dependencies(pictor_sharc_vulkan pictor_sharc_shaders)
