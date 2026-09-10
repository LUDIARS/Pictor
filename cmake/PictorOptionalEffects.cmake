# Optional host-driven effects. Core exposes value-only profile contracts;
# consumers explicitly link executable effect implementations.
option(PICTOR_ENABLE_POSTPROCESS "Build the optional postprocess module" ON)
option(PICTOR_ENABLE_DECALS "Build the optional projected decal module" ON)
include(${CMAKE_CURRENT_LIST_DIR}/PictorEffectShaders.cmake)

if(PICTOR_ENABLE_POSTPROCESS)
    add_library(pictor_postprocess STATIC
        src/postprocess/postprocess_chain.cpp
        src/postprocess/postprocess_config_bridge.cpp
        src/postprocess/postprocess_pipeline.cpp)
    add_library(Pictor::postprocess ALIAS pictor_postprocess)
    target_link_libraries(pictor_postprocess PUBLIC pictor)
    target_compile_definitions(pictor_postprocess PUBLIC PICTOR_HAS_POSTPROCESS=1)
    if(MSVC)
        target_compile_options(pictor_postprocess PRIVATE /W4 /utf-8)
    endif()
    if(Vulkan_FOUND)
        include(${CMAKE_CURRENT_LIST_DIR}/PictorPostprocessShaders.cmake)
    endif()
endif()

if(PICTOR_ENABLE_DECALS)
    add_library(pictor_decals STATIC src/decal/decal_system.cpp)
    add_library(Pictor::decals ALIAS pictor_decals)
    target_link_libraries(pictor_decals PUBLIC pictor)
    target_compile_definitions(pictor_decals PUBLIC PICTOR_HAS_DECALS=1)
    if(MSVC)
        target_compile_options(pictor_decals PRIVATE /W4 /utf-8)
    endif()
    if(Vulkan_FOUND)
        pictor_effect_shaders(pictor_decals
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/decal/decal.frag;${CMAKE_CURRENT_SOURCE_DIR}/shaders/postprocess/fullscreen_quad.vert"
            "${CMAKE_CURRENT_BINARY_DIR}/shaders/decal")
    endif()
endif()
