# Optional font-vector text: glyph outlines -> cached triangle meshes ->
# instanced draws. No glyph atlas or SDF. Consumers link Pictor::vector_text.
# spec/feature/vector-text-module.md
option(PICTOR_ENABLE_VECTOR_TEXT "Build the optional vector text module" ON)

if(PICTOR_ENABLE_VECTOR_TEXT)
    add_library(pictor_vector_text STATIC
        src/vector_text/glyph_contours.cpp
        src/vector_text/nonzero_tessellator.cpp
        src/vector_text/glyph_feather.cpp
        src/vector_text/size_tier.cpp
        src/vector_text/glyph_mesh_builder.cpp
        src/vector_text/glyph_outline_source.cpp
        src/vector_text/glyph_mesh_cache.cpp
        src/vector_text/vector_text_batch.cpp
        src/vector_text/vector_text_layout.cpp)
    add_library(Pictor::vector_text ALIAS pictor_vector_text)
    target_link_libraries(pictor_vector_text PUBLIC pictor)
    target_compile_definitions(pictor_vector_text PUBLIC PICTOR_HAS_VECTOR_TEXT=1)
    if(MSVC)
        target_compile_options(pictor_vector_text PRIVATE /W4 /utf-8)
    endif()
    if(Vulkan_FOUND)
        target_sources(pictor_vector_text PRIVATE
            src/vector_text/vulkan/host_buffer.cpp
            src/vector_text/vulkan/vector_text_pipeline.cpp
            src/vector_text/vulkan/vector_text_renderer.cpp)
        include(${CMAKE_CURRENT_LIST_DIR}/PictorEffectShaders.cmake)
        pictor_effect_shaders(pictor_vector_text
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vector_text/vector_text.vert;${CMAKE_CURRENT_SOURCE_DIR}/shaders/vector_text/vector_text.frag"
            "${CMAKE_CURRENT_BINARY_DIR}/shaders/vector_text")
    endif()
endif()
