# SHaRC is an optional leaf module. Consumers select a backend explicitly;
# enabling a module never adds its implementation to the core pictor archive.
option(PICTOR_ENABLE_SHARC "Enable optional SHaRC modules" ON)
option(PICTOR_ENABLE_SHARC_VULKAN "Build SHaRC Vulkan executor" ${Vulkan_FOUND})
option(PICTOR_ENABLE_SHARC_DX12 "Build SHaRC DirectX 12 executor" ${WIN32})

# The master switch overrides cached backend selections, so switching an
# existing build tree OFF also removes every SHaRC target and shader rule.
if(NOT PICTOR_ENABLE_SHARC)
    return()
endif()

add_library(pictor_sharc_contract INTERFACE)
add_library(Pictor::sharc_contract ALIAS pictor_sharc_contract)
target_include_directories(pictor_sharc_contract INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(pictor_sharc_contract INTERFACE cxx_std_20)

if(PICTOR_ENABLE_SHARC_VULKAN)
    if(NOT Vulkan_FOUND)
        message(FATAL_ERROR "PICTOR_ENABLE_SHARC_VULKAN=ON requires a Vulkan backend; disable it for a non-Vulkan consumer")
    endif()
    add_library(pictor_sharc_vulkan STATIC src/gi/sharc_executor.cpp)
    add_library(Pictor::sharc_vulkan ALIAS pictor_sharc_vulkan)
    target_link_libraries(pictor_sharc_vulkan PUBLIC pictor Pictor::sharc_contract)
    target_compile_definitions(pictor_sharc_vulkan PUBLIC PICTOR_HAS_SHARC_VULKAN=1)
    if(MSVC)
        target_compile_options(pictor_sharc_vulkan PRIVATE /W4 /utf-8)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(pictor_sharc_vulkan PRIVATE -Wall -Wextra)
    endif()
    include(${CMAKE_CURRENT_LIST_DIR}/PictorSharcShaders.cmake)
endif()

if(PICTOR_ENABLE_SHARC_DX12)
    if(NOT WIN32)
        message(FATAL_ERROR "PICTOR_ENABLE_SHARC_DX12=ON requires Windows")
    endif()
    add_library(pictor_sharc_dx12 STATIC src/gi/sharc_dx12_executor.cpp)
    add_library(Pictor::sharc_dx12 ALIAS pictor_sharc_dx12)
    target_link_libraries(pictor_sharc_dx12 PUBLIC
        Pictor::sharc_contract d3d12 dxgi d3dcompiler)
    target_compile_definitions(pictor_sharc_dx12 PUBLIC PICTOR_HAS_SHARC_DX12=1 NOMINMAX=1)
    if(MSVC)
        target_compile_options(pictor_sharc_dx12 PRIVATE /W4 /utf-8)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(pictor_sharc_dx12 PRIVATE -Wall -Wextra)
    endif()
    # Runtime compilation remains owned by the executor. No demo path or
    # process working directory is embedded into the library.
    set(PICTOR_SHARC_HLSL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders/sharc/hlsl")
endif()
