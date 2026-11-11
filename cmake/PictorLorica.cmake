# Lorica (LUDIARS defensive library) for every target Pictor compiles itself.
#
#   - Lorica::boundary (header-only checked sizes / byte reading / budgets) is linked
#     PRIVATE through $<BUILD_INTERFACE:...>: no Pictor public header includes Lorica,
#     so neither a consumer's add_subdirectory nor the PolynomialDemo install export
#     needs Lorica in its own link interface.
#   - lorica_harden_target (baseline) on every locally compiled Pictor target. Pictor is a
#     per-frame rendering library, so nothing here takes EXTENDED: no Pictor executable is a
#     resident server or network endpoint (spec/plan/2026-11-11-lorica-adoption.md).
#   - lorica_deprecate_unsafe_apis where Lorica supports it on this toolchain.
#   - Source-built third-party libraries (GLFW / OpenXR loader fetched by FetchContent)
#     get the baseline profile from here; their sources are not edited.
#
# Lorica's hardening supports Windows MSVC-compatible toolchains and Linux GCC/Clang only.
# Elsewhere (Emscripten, Android NDK, iOS, macOS, MinGW) the hardening is skipped and
# configure says so with a WARNING; the boundary headers are still compiled in.
#
# A Pictor-owned compiled target defined after pictor_lorica_apply_all() fails configure.
#
# Local checkout: -DLORICA_SOURCE_DIR=/path/to/Lorica. Otherwise the pinned commit is fetched.
# When the including project already provides Lorica (Lorica::boundary exists, from its own
# add_subdirectory / FetchContent / find_package), Pictor uses that one and adds nothing.

include_guard(GLOBAL)

set(PICTOR_LORICA_COMMIT 5e8281b0e60fa16f814e16eaa620715937e8618d)
set(LORICA_SOURCE_DIR "" CACHE PATH "Local Lorica checkout; empty fetches the pinned commit")

if(TARGET Lorica::boundary)
    message(STATUS "Pictor: using the Lorica provided by the including project")
else()
    # Normal variables (CMP0077): Lorica's tests, fuzzer and compiled protection component
    # are not part of Pictor's build. No cache entry of the including project is forced.
    set(LORICA_BUILD_PROTECTION OFF)
    set(LORICA_BUILD_TESTS OFF)
    set(LORICA_BUILD_FUZZER OFF)
    if(LORICA_SOURCE_DIR)
        add_subdirectory("${LORICA_SOURCE_DIR}" "${PROJECT_BINARY_DIR}/_deps/lorica-build" EXCLUDE_FROM_ALL)
    else()
        include(FetchContent)
        FetchContent_Declare(lorica
            GIT_REPOSITORY https://github.com/LUDIARS/Lorica.git
            GIT_TAG        ${PICTOR_LORICA_COMMIT})
        FetchContent_MakeAvailable(lorica)
    endif()
endif()
if(NOT TARGET Lorica::boundary OR NOT COMMAND lorica_harden_target)
    message(FATAL_ERROR "Pictor: Lorica did not provide Lorica::boundary and lorica_harden_target")
endif()

# Which hardening this toolchain can take. Mirrors Lorica's own platform test
# (LoricaHardening.cmake, _lorica_apply_baseline): anything else would be a FATAL_ERROR there.
if((MSVC AND WIN32) OR
   (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$"))
    set(PICTOR_LORICA_HARDENING TRUE)
    set(PICTOR_LORICA_HARDENING_REASON "")
    lorica_unsafe_api_rejection_supported(PICTOR_LORICA_UNSAFE_API)
else()
    set(PICTOR_LORICA_HARDENING FALSE)
    set(PICTOR_LORICA_HARDENING_REASON
        "Lorica hardening supports Windows MSVC-compatible toolchains and Linux GCC/Clang only, not ${CMAKE_SYSTEM_NAME}/${CMAKE_CXX_COMPILER_ID}")
    set(PICTOR_LORICA_UNSAFE_API FALSE)
    set(PICTOR_LORICA_UNSAFE_API_REASON "${PICTOR_LORICA_HARDENING_REASON}")
endif()

option(PICTOR_LORICA_HARDEN_FETCHED_DEPS
       "Apply Lorica's baseline hardening to third-party libraries Pictor builds from source (GLFW, OpenXR loader)" ON)

# Directories whose targets Pictor owns: the Pictor source tree and its subdirectories,
# except third_party/ and anything fetched into the binary tree.
function(_pictor_lorica_own_directories dir out)
    set(result "${dir}")
    get_property(children DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(child IN LISTS children)
        cmake_path(IS_PREFIX PROJECT_SOURCE_DIR "${child}" NORMALIZE inside_source)
        cmake_path(IS_PREFIX PROJECT_BINARY_DIR "${child}" NORMALIZE inside_binary)
        cmake_path(IS_PREFIX PICTOR_LORICA_THIRD_PARTY_DIR "${child}" NORMALIZE vendored)
        if(inside_source AND NOT inside_binary AND NOT vendored)
            _pictor_lorica_own_directories("${child}" nested)
            list(APPEND result ${nested})
        endif()
    endforeach()
    set(${out} "${result}" PARENT_SCOPE)
endfunction()
set(PICTOR_LORICA_THIRD_PARTY_DIR "${PROJECT_SOURCE_DIR}/third_party")

# Every compiled target in a Pictor-owned directory (also used by PictorSanitizers.cmake).
function(pictor_own_compiled_targets out)
    _pictor_lorica_own_directories("${PROJECT_SOURCE_DIR}" dirs)
    set(result "")
    foreach(dir IN LISTS dirs)
        get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
        foreach(target IN LISTS targets)
            get_target_property(kind "${target}" TYPE)
            if(kind MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
                list(APPEND result "${target}")
            endif()
        endforeach()
    endforeach()
    set(${out} "${result}" PARENT_SCOPE)
endfunction()

# Lorica on one Pictor-owned target.
function(pictor_lorica_target target)
    get_target_property(done "${target}" PICTOR_LORICA_APPLIED)
    if(done)
        return()
    endif()
    target_link_libraries("${target}" PRIVATE "$<BUILD_INTERFACE:Lorica::boundary>")
    if(PICTOR_LORICA_HARDENING)
        lorica_harden_target("${target}")
        if(PICTOR_LORICA_UNSAFE_API)
            lorica_deprecate_unsafe_apis("${target}")
        endif()
    endif()
    set_property(TARGET "${target}" PROPERTY PICTOR_LORICA_APPLIED TRUE)
endfunction()

# Baseline hardening on a third-party library compiled from source in this build.
# Unsafe API rejection is not applied: GLFW is C, which Lorica's rejection does not cover.
function(_pictor_lorica_third_party target)
    if(NOT TARGET "${target}")
        return()
    endif()
    get_target_property(imported "${target}" IMPORTED)
    get_target_property(aliased "${target}" ALIASED_TARGET)
    if(imported)
        message(STATUS "Pictor: ${target} is a prebuilt (imported) library; Lorica cannot harden it")
        return()
    endif()
    if(aliased)
        set(target "${aliased}")
    endif()
    get_target_property(hardened "${target}" LORICA_HARDENED)
    if(hardened OR NOT PICTOR_LORICA_HARDENING)
        return()
    endif()
    if(NOT PICTOR_LORICA_HARDEN_FETCHED_DEPS)
        message(WARNING "Pictor: PICTOR_LORICA_HARDEN_FETCHED_DEPS=OFF leaves source-built ${target} without Lorica hardening")
        return()
    endif()
    lorica_harden_target("${target}")
    message(STATUS "Pictor: Lorica baseline hardening applied to source-built ${target}")
endfunction()

# Call once, after every Pictor target of this configure exists.
function(pictor_lorica_apply_all)
    pictor_own_compiled_targets(targets)
    foreach(target IN LISTS targets)
        pictor_lorica_target("${target}")
    endforeach()
    _pictor_lorica_third_party(glfw)
    _pictor_lorica_third_party(openxr_loader)
    list(LENGTH targets count)
    if(PICTOR_LORICA_HARDENING)
        message(STATUS "Pictor: Lorica baseline hardening on ${count} Pictor targets")
        if(NOT PICTOR_LORICA_UNSAFE_API)
            message(WARNING "Pictor: Lorica unsafe C API rejection is not applied: ${PICTOR_LORICA_UNSAFE_API_REASON}")
        endif()
    else()
        message(WARNING "Pictor: Lorica hardening is NOT applied to ${count} Pictor targets: ${PICTOR_LORICA_HARDENING_REASON}. "
                        "Only the Lorica::boundary checks are compiled in for this toolchain.")
    endif()
    set_property(GLOBAL PROPERTY PICTOR_LORICA_APPLIED_ALL TRUE)
    cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}" CALL _pictor_lorica_check_all)
endfunction()

# Deferred to the end of Pictor's top directory: no Pictor-owned compiled target may
# have been added after pictor_lorica_apply_all() without Lorica.
function(_pictor_lorica_check_all)
    pictor_own_compiled_targets(targets)
    set(missing "")
    foreach(target IN LISTS targets)
        get_target_property(done "${target}" PICTOR_LORICA_APPLIED)
        if(NOT done)
            list(APPEND missing "${target}")
        endif()
    endforeach()
    if(missing)
        message(FATAL_ERROR "Pictor: targets without Lorica (define them before pictor_lorica_apply_all() or call pictor_lorica_target): ${missing}")
    endif()
endfunction()
