# Each effect owns shader outputs. Distinct output directories avoid duplicate
# producers when two optional modules use the same fullscreen vertex shader.
function(pictor_effect_shaders target sources output_dir)
    if(NOT Vulkan_GLSLC_EXECUTABLE)
        message(STATUS "${target}: supply precompiled SPIR-V to the host (glslc unavailable)")
        return()
    endif()
    set(outputs)
    foreach(source IN LISTS sources)
        get_filename_component(name "${source}" NAME)
        set(output "${output_dir}/${name}.spv")
        add_custom_command(OUTPUT "${output}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${output_dir}"
            COMMAND "${Vulkan_GLSLC_EXECUTABLE}" "${source}" -o "${output}"
            DEPENDS "${source}"
            VERBATIM)
        list(APPEND outputs "${output}")
    endforeach()
    add_custom_target(${target}_shaders DEPENDS ${outputs})
    add_dependencies(${target} ${target}_shaders)
endfunction()
