# FindRive.cmake
#
# Locates a pre-built rive-runtime distribution (https://github.com/rive-app/rive-runtime)
# produced by its native premake5 build (`./build.sh release`).
#
# Rive itself does not ship a CMake build, so Pictor treats it as an external
# prebuilt dependency. The user is expected to clone rive-runtime, run its
# build once, and point this module at the result via PICTOR_RIVE_DIR.
#
# Inputs (set by the caller or on the cmake command line):
#   PICTOR_RIVE_DIR    — root of the rive-runtime checkout (must contain
#                        include/ and the compiled static libs under out/).
#   PICTOR_RIVE_CONFIG — build config to consume ("debug" or "release",
#                        default: "release").
#
# Outputs:
#   Rive_FOUND              — TRUE if all required libraries were located.
#   Rive_INCLUDE_DIRS       — list of include directories.
#   Rive_LIBRARIES          — list of static libraries to link.
#   Rive::Rive              — imported INTERFACE target wrapping the above.
#
# Expected on-disk layout (matches rive-runtime's premake defaults):
#   ${PICTOR_RIVE_DIR}/include/
#   ${PICTOR_RIVE_DIR}/renderer/include/
#   ${PICTOR_RIVE_DIR}/out/${PICTOR_RIVE_CONFIG}/rive.lib
#   ${PICTOR_RIVE_DIR}/out/${PICTOR_RIVE_CONFIG}/rive_pls_renderer.lib   (or rive_renderer)
#   ${PICTOR_RIVE_DIR}/out/${PICTOR_RIVE_CONFIG}/rive_vk_bootstrap.lib
#   ${PICTOR_RIVE_DIR}/out/${PICTOR_RIVE_CONFIG}/rive_decoders.lib        (optional)
#
# On non-Windows platforms the extension is .a instead of .lib.

if(NOT PICTOR_RIVE_DIR)
    set(Rive_FOUND FALSE)
    return()
endif()

if(NOT PICTOR_RIVE_CONFIG)
    set(PICTOR_RIVE_CONFIG "release")
endif()

# rive-runtime's premake places build artifacts under the subproject
# directory whose premake5.lua was invoked. For the renderer (the library
# we care about here) that is `renderer/out/<config>/`. A handful of
# users drive the monorepo-level build instead, which places artifacts
# at `out/<config>/` — check both so either workflow is supported.
set(_rive_lib_candidates
    "${PICTOR_RIVE_DIR}/renderer/out/${PICTOR_RIVE_CONFIG}"
    "${PICTOR_RIVE_DIR}/out/${PICTOR_RIVE_CONFIG}"
)
set(_rive_lib_dir "")
foreach(_dir IN LISTS _rive_lib_candidates)
    if(EXISTS "${_dir}")
        set(_rive_lib_dir "${_dir}")
        break()
    endif()
endforeach()

# Include directories — Rive's public headers live in two trees:
#   include/               -> rive/core/*, rive/animation/*, scene graph
#   renderer/include/      -> rive/renderer/* (the GPU renderer public API)
set(Rive_INCLUDE_DIRS
    "${PICTOR_RIVE_DIR}/include"
    "${PICTOR_RIVE_DIR}/renderer/include"
)

# Primary libraries. rive-runtime's premake output names vary slightly
# between branches (rive_pls_renderer was renamed to rive_renderer in the
# public-domain release); probe both.
find_library(Rive_CORE_LIB
    NAMES rive
    PATHS "${_rive_lib_dir}"
    NO_DEFAULT_PATH
)
find_library(Rive_RENDERER_LIB
    NAMES rive_renderer rive_pls_renderer
    PATHS "${_rive_lib_dir}"
    NO_DEFAULT_PATH
)
find_library(Rive_DECODERS_LIB
    NAMES rive_decoders
    PATHS "${_rive_lib_dir}"
    NO_DEFAULT_PATH
)
find_library(Rive_HARFBUZZ_LIB
    NAMES rive_harfbuzz harfbuzz
    PATHS "${_rive_lib_dir}"
    NO_DEFAULT_PATH
)
find_library(Rive_SHEENBIDI_LIB
    NAMES rive_sheenbidi sheenbidi
    PATHS "${_rive_lib_dir}"
    NO_DEFAULT_PATH
)
find_library(Rive_YOGA_LIB
    NAMES rive_yoga yoga
    PATHS "${_rive_lib_dir}"
    NO_DEFAULT_PATH
)

# Image decoder transitive deps — rive_decoders.lib pulls unresolved symbols
# from libjpeg, libpng, libwebp, and zlib, all built alongside Rive under the
# same output directory.
find_library(Rive_LIBJPEG_LIB NAMES libjpeg jpeg PATHS "${_rive_lib_dir}" NO_DEFAULT_PATH)
find_library(Rive_LIBPNG_LIB  NAMES libpng png   PATHS "${_rive_lib_dir}" NO_DEFAULT_PATH)
find_library(Rive_LIBWEBP_LIB NAMES libwebp webp PATHS "${_rive_lib_dir}" NO_DEFAULT_PATH)
find_library(Rive_ZLIB_LIB    NAMES zlib z       PATHS "${_rive_lib_dir}" NO_DEFAULT_PATH)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Rive
    REQUIRED_VARS
        Rive_CORE_LIB
        Rive_RENDERER_LIB
    FAIL_MESSAGE
        "Rive runtime not found. Set PICTOR_RIVE_DIR to the root of a checked-out and built rive-runtime (run `./build.sh release` inside rive-runtime first)."
)

if(Rive_FOUND)
    set(Rive_LIBRARIES
        ${Rive_RENDERER_LIB}
        ${Rive_CORE_LIB}
    )
    # Optional auxiliaries — include only if present. Order matters for
    # static-link resolution: decoders first (they reference the image libs),
    # harfbuzz/sheenbidi/yoga provide text/layout symbols called from rive.lib,
    # image libs last since nothing depends on them.
    foreach(_lib IN ITEMS
            Rive_DECODERS_LIB
            Rive_HARFBUZZ_LIB
            Rive_SHEENBIDI_LIB
            Rive_YOGA_LIB
            Rive_LIBJPEG_LIB
            Rive_LIBPNG_LIB
            Rive_LIBWEBP_LIB
            Rive_ZLIB_LIB)
        if(${_lib})
            list(APPEND Rive_LIBRARIES ${${_lib}})
        endif()
    endforeach()

    if(NOT TARGET Rive::Rive)
        add_library(Rive::Rive INTERFACE IMPORTED)
        set_target_properties(Rive::Rive PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES   "${Rive_INCLUDE_DIRS}"
            INTERFACE_LINK_LIBRARIES        "${Rive_LIBRARIES}"
            # Must mirror the defines Rive itself is compiled with so the
            # Vulkan class declarations (gated on RIVE_VULKAN) and the
            # rive-render-factory (gated on WITH_RIVE_*) are visible to
            # consumers. Missing these yields spurious "incomplete type"
            # errors on RenderContextVulkanImpl.
            INTERFACE_COMPILE_DEFINITIONS   "RIVE_VULKAN;WITH_RIVE_TEXT;WITH_RIVE_LAYOUT"
        )
    endif()
endif()
