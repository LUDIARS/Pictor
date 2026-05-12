# FindRive.cmake
#
# Locates a pre-built rive-runtime distribution (https://github.com/rive-app/rive-runtime)
# produced by its native premake5 build (`./build.sh release` / `./build.sh debug`).
#
# Rive itself does not ship a CMake build, so Pictor treats it as an external
# prebuilt dependency. The user is expected to clone rive-runtime, run its
# build once (per configuration), and point this module at the result via
# PICTOR_RIVE_DIR.
#
# Inputs:
#   PICTOR_RIVE_DIR    — root of the rive-runtime checkout (must contain
#                        renderer/out/<config>/ with compiled static libs).
#   PICTOR_RIVE_CONFIG — single-config fallback; one of "release", "debug".
#                        Only used when the generator is single-config
#                        (Unix Makefiles, Ninja) and the consumer hasn't
#                        built both. Default: "release".
#
# Multi-config generators (Visual Studio, Xcode) look up **both** debug and
# release artifacts and select per configuration via CMake imported-target
# config mapping. To fully light up, build rive-runtime twice:
#
#   # Windows (Git Bash):
#   cd rive-runtime/renderer
#   ../build/build_rive.bat release --with_vulkan --toolset=msc --windows_runtime=dynamic_release
#   ../build/build_rive.bat debug   --with_vulkan --toolset=msc --windows_runtime=dynamic_debug
#
# Outputs:
#   Rive_FOUND           — TRUE if all required libraries were located.
#   Rive_INCLUDE_DIRS    — list of include directories.
#   Rive_LIBRARIES       — list of static libraries (may contain generator
#                          expressions on multi-config).
#   Rive::Rive           — imported INTERFACE target wrapping the above
#                          with per-config link libraries.

if(NOT PICTOR_RIVE_DIR)
    set(Rive_FOUND FALSE)
    return()
endif()

if(NOT PICTOR_RIVE_CONFIG)
    set(PICTOR_RIVE_CONFIG "release")
endif()

# rive-runtime's premake places build artifacts under the subproject directory
# whose premake5.lua was invoked. For the renderer that is `renderer/out/<cfg>/`.
# Also probe `out/<cfg>/` for consumers who drive the monorepo-level build.
set(_rive_release_candidates
    "${PICTOR_RIVE_DIR}/renderer/out/release"
    "${PICTOR_RIVE_DIR}/out/release"
)
set(_rive_debug_candidates
    "${PICTOR_RIVE_DIR}/renderer/out/debug"
    "${PICTOR_RIVE_DIR}/out/debug"
)

set(_rive_release_dir "")
foreach(_dir IN LISTS _rive_release_candidates)
    if(EXISTS "${_dir}")
        set(_rive_release_dir "${_dir}")
        break()
    endif()
endforeach()

set(_rive_debug_dir "")
foreach(_dir IN LISTS _rive_debug_candidates)
    if(EXISTS "${_dir}")
        set(_rive_debug_dir "${_dir}")
        break()
    endif()
endforeach()

# For single-config generators, fall back to the chosen PICTOR_RIVE_CONFIG.
set(_rive_fallback_dir "")
if("${PICTOR_RIVE_CONFIG}" STREQUAL "debug")
    set(_rive_fallback_dir "${_rive_debug_dir}")
else()
    set(_rive_fallback_dir "${_rive_release_dir}")
endif()

set(Rive_INCLUDE_DIRS
    "${PICTOR_RIVE_DIR}/include"
    "${PICTOR_RIVE_DIR}/renderer/include"
)

# ─── Per-library discovery ────────────────────────────────────

set(_rive_required_components CORE RENDERER)
set(_rive_missing "")

# Helper: probe a single library by-name in both release and debug dirs and
# populate Rive_<VAR>_LIB_{RELEASE,DEBUG}. Accepts multiple name aliases
# (e.g. current rive_renderer and historical rive_pls_renderer).
function(_rive_find_pair var_root)
    set(_names ${ARGN})
    if(_rive_release_dir)
        find_library(Rive_${var_root}_LIB_RELEASE
            NAMES ${_names}
            PATHS "${_rive_release_dir}"
            NO_DEFAULT_PATH
        )
    else()
        unset(Rive_${var_root}_LIB_RELEASE CACHE)
    endif()
    if(_rive_debug_dir)
        find_library(Rive_${var_root}_LIB_DEBUG
            NAMES ${_names}
            PATHS "${_rive_debug_dir}"
            NO_DEFAULT_PATH
        )
    else()
        unset(Rive_${var_root}_LIB_DEBUG CACHE)
    endif()
endfunction()

_rive_find_pair(CORE      rive)
_rive_find_pair(RENDERER  rive_renderer rive_pls_renderer)
_rive_find_pair(DECODERS  rive_decoders)
_rive_find_pair(HARFBUZZ  rive_harfbuzz harfbuzz)
_rive_find_pair(SHEENBIDI rive_sheenbidi sheenbidi)
_rive_find_pair(YOGA      rive_yoga yoga)
_rive_find_pair(LIBJPEG   libjpeg jpeg)
_rive_find_pair(LIBPNG    libpng png)
_rive_find_pair(LIBWEBP   libwebp webp)
_rive_find_pair(ZLIB      zlib z)

# ─── Validation ──────────────────────────────────────────────

foreach(_comp IN LISTS _rive_required_components)
    set(_release_var "Rive_${_comp}_LIB_RELEASE")
    set(_debug_var   "Rive_${_comp}_LIB_DEBUG")
    if(NOT ${_release_var} AND NOT ${_debug_var})
        list(APPEND _rive_missing "${_comp}")
    endif()
endforeach()

include(FindPackageHandleStandardArgs)
if(_rive_missing)
    set(Rive_FOUND FALSE)
    find_package_handle_standard_args(Rive
        REQUIRED_VARS _rive_release_dir
        FAIL_MESSAGE
            "Rive runtime not found. Set PICTOR_RIVE_DIR to the root of a "
            "checked-out and built rive-runtime. For multi-config generators "
            "(Visual Studio), build both configurations with:\n"
            "  ../build/build_rive.bat release --with_vulkan --toolset=msc --windows_runtime=dynamic_release\n"
            "  ../build/build_rive.bat debug   --with_vulkan --toolset=msc --windows_runtime=dynamic_debug\n"
            "Missing components: ${_rive_missing}"
    )
    return()
endif()

set(Rive_FOUND TRUE)

# ─── Build per-config library lists ──────────────────────────

# Fixed link order: renderer → core → decoders → text → layout → image libs.
set(_rive_link_order CORE RENDERER DECODERS HARFBUZZ SHEENBIDI YOGA LIBJPEG LIBPNG LIBWEBP ZLIB)
# Link order matters for static archives: decoders references the image
# libs, rive-core references harfbuzz/sheenbidi/yoga, so put leaves last.
set(_rive_link_order RENDERER CORE DECODERS HARFBUZZ SHEENBIDI YOGA LIBJPEG LIBPNG LIBWEBP ZLIB)

set(Rive_LIBRARIES_RELEASE "")
set(Rive_LIBRARIES_DEBUG   "")
foreach(_comp IN LISTS _rive_link_order)
    if(Rive_${_comp}_LIB_RELEASE)
        list(APPEND Rive_LIBRARIES_RELEASE ${Rive_${_comp}_LIB_RELEASE})
    endif()
    if(Rive_${_comp}_LIB_DEBUG)
        list(APPEND Rive_LIBRARIES_DEBUG ${Rive_${_comp}_LIB_DEBUG})
    endif()
endforeach()

# Fallback — if only one config exists, reuse it for the other.
if(NOT Rive_LIBRARIES_DEBUG)
    set(Rive_LIBRARIES_DEBUG ${Rive_LIBRARIES_RELEASE})
endif()
if(NOT Rive_LIBRARIES_RELEASE)
    set(Rive_LIBRARIES_RELEASE ${Rive_LIBRARIES_DEBUG})
endif()

# ─── Imported target ─────────────────────────────────────────

if(NOT TARGET Rive::Rive)
    add_library(Rive::Rive INTERFACE IMPORTED)
    set_target_properties(Rive::Rive PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${Rive_INCLUDE_DIRS}"
        # Must mirror the defines Rive itself is compiled with so the Vulkan
        # class declarations (gated on RIVE_VULKAN) and the rive-render-factory
        # (gated on WITH_RIVE_*) are visible to consumers.
        INTERFACE_COMPILE_DEFINITIONS "RIVE_VULKAN;WITH_RIVE_TEXT;WITH_RIVE_LAYOUT"
    )
    # Per-config link libraries. We wrap each path individually so the list's
    # `;` separators don't confuse CMake's generator-expression parser —
    # collapsing the whole list into a single `$<CONFIG:...>` expression
    # produced an empty expansion in practice (dropped libs → LNK2001).
    set(_rive_link_gex "")
    foreach(_lib IN LISTS Rive_LIBRARIES_RELEASE)
        list(APPEND _rive_link_gex "$<$<NOT:$<CONFIG:Debug>>:${_lib}>")
    endforeach()
    foreach(_lib IN LISTS Rive_LIBRARIES_DEBUG)
        list(APPEND _rive_link_gex "$<$<CONFIG:Debug>:${_lib}>")
    endforeach()
    set_property(TARGET Rive::Rive PROPERTY
        INTERFACE_LINK_LIBRARIES ${_rive_link_gex}
    )
endif()

# Legacy variable kept for consumers still using raw Rive_LIBRARIES.
# Defaults to the fallback config that matches PICTOR_RIVE_CONFIG.
if("${PICTOR_RIVE_CONFIG}" STREQUAL "debug")
    set(Rive_LIBRARIES ${Rive_LIBRARIES_DEBUG})
else()
    set(Rive_LIBRARIES ${Rive_LIBRARIES_RELEASE})
endif()
