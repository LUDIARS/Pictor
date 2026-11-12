# Tests that fail when Lorica is not actually applied to the build (cmake/PictorLorica.cmake).
#
# The configure-time coverage check (_pictor_lorica_check_all: a Pictor target without Lorica
# is a FATAL_ERROR) stays; these look at what the build produced:
#
#   lorica_applied_tu_test                 a unit compiled like every Pictor target checks,
#                                          from inside, the forced unsafe API header, the
#                                          standard library checks, FORTIFY, stack protector,
#                                          CET / PIE, and runs Lorica::boundary.
#   lorica_applied_compile_commands_test   cmake -P over compile_commands.json: every Pictor
#                                          unit carries Lorica's include and baseline flags and
#                                          nothing cancels them; source-built GLFW (C) carries
#                                          the C baseline.
#   lorica_applied_binary_<target>_test    Lorica's tools/verify_binary.py on Pictor's
#                                          executables outside tests/ (only with python3).
#
# The expectations are derived from the toolchain the way LoricaHardening.cmake (pinned
# commit) decides them, not read back from the properties Lorica sets: a target that lost its
# Lorica wiring must not lower its own expectations.

set(_lorica_skip "PICTOR_LORICA_SKIP:")

# --- what Lorica's baseline puts on a target here -------------------------------------------
get_property(_lorica_unsafe_header GLOBAL PROPERTY LORICA_UNSAFE_API_HEADER)
if(NOT EXISTS "${_lorica_unsafe_header}")
    message(FATAL_ERROR "Pictor: Lorica's lorica/unsafe_api.hpp is not known (LORICA_UNSAFE_API_HEADER)")
endif()
cmake_path(GET _lorica_unsafe_header PARENT_PATH _lorica_include_dir)   # .../include/lorica
cmake_path(GET _lorica_include_dir PARENT_PATH _lorica_include_dir)      # .../include
cmake_path(GET _lorica_include_dir PARENT_PATH _lorica_root)

set(_lorica_linux FALSE)
if(PICTOR_LORICA_HARDENING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_lorica_linux TRUE)
endif()
foreach(_lang C CXX)
    set(_lorica_fortify_${_lang} "")
    if(_lorica_linux AND CMAKE_${_lang}_COMPILER_LOADED)
        if(CMAKE_${_lang}_COMPILER_VERSION VERSION_GREATER_EQUAL 12)
            set(_lorica_fortify_${_lang} 3)
        else()
            set(_lorica_fortify_${_lang} 2)
        endif()
    endif()
endforeach()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(_lorica_cfp_flag "-mbranch-protection=standard")
else()
    set(_lorica_cfp_flag "-fcf-protection=full")
endif()
set(_lorica_optimized_configs Release RelWithDebInfo MinSizeRel)

# --- 1. inside a translation unit -----------------------------------------------------------
add_executable(lorica_applied_tu_test lorica_applied_tu_test.cpp)
target_include_directories(lorica_applied_tu_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_definitions(lorica_applied_tu_test PRIVATE
    PICTOR_LORICA_EXPECT_HARDENING=$<BOOL:${PICTOR_LORICA_HARDENING}>
    PICTOR_LORICA_EXPECT_UNSAFE_API=$<BOOL:${PICTOR_LORICA_UNSAFE_API}>)
if(_lorica_fortify_CXX)
    target_compile_definitions(lorica_applied_tu_test PRIVATE
        "$<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:PICTOR_LORICA_EXPECT_FORTIFY=${_lorica_fortify_CXX}>")
endif()
if(MSVC)
    target_compile_options(lorica_applied_tu_test PRIVATE /utf-8)
endif()
add_test(NAME lorica_applied_tu_test COMMAND lorica_applied_tu_test
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(lorica_applied_tu_test PROPERTIES
    TIMEOUT 60 SKIP_REGULAR_EXPRESSION "${_lorica_skip}")

# --- 2. every compile command ---------------------------------------------------------------
get_property(_lorica_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
set(_lorica_reason "")
if(NOT PICTOR_LORICA_HARDENING)
    set(_lorica_mode unsupported)
    set(_lorica_reason "Pictor builds without Lorica hardening here: ${PICTOR_LORICA_HARDENING_REASON}")
elseif(CMAKE_GENERATOR MATCHES "^(Visual Studio|Xcode)")
    set(_lorica_mode no-export)
    set(_lorica_reason "the ${CMAKE_GENERATOR} generator does not write compile_commands.json; the flags cannot be read")
elseif(NOT CMAKE_EXPORT_COMPILE_COMMANDS)
    set(_lorica_mode no-export)
    set(_lorica_reason "CMAKE_EXPORT_COMPILE_COMMANDS is OFF (Pictor enables it only as the top-level project)")
elseif(MSVC)
    set(_lorica_mode msvc)
else()
    set(_lorica_mode gnu)
endif()

# Single-config: FORTIFY is expected when the build type is an optimized one. A multi-config
# generator writes no configuration per entry, so the level is not checked there.
set(_lorica_cc_fortify_C "")
set(_lorica_cc_fortify_CXX "")
if(_lorica_multi)
    message(STATUS "Pictor: lorica_applied_compile_commands_test does not check _FORTIFY_SOURCE with a multi-config generator")
elseif(CMAKE_BUILD_TYPE IN_LIST _lorica_optimized_configs)
    set(_lorica_cc_fortify_C "${_lorica_fortify_C}")
    set(_lorica_cc_fortify_CXX "${_lorica_fortify_CXX}")
endif()

set(_lorica_unsafe_expected "")
if(PICTOR_LORICA_UNSAFE_API)
    set(_lorica_unsafe_expected "${_lorica_unsafe_header}")
endif()

# GLFW fetched and compiled here (third-party C): baseline only.
set(_lorica_glfw_dir "")
if(TARGET glfw AND glfw_SOURCE_DIR)
    get_target_property(_lorica_glfw_imported glfw IMPORTED)
    if(NOT _lorica_glfw_imported)
        if(PICTOR_LORICA_HARDEN_FETCHED_DEPS)
            set(_lorica_glfw_dir "${glfw_SOURCE_DIR}")
        else()
            message(STATUS "Pictor: PICTOR_LORICA_HARDEN_FETCHED_DEPS=OFF, so lorica_applied_compile_commands_test does not check GLFW")
        endif()
    endif()
endif()

set(_lorica_excluded "${PROJECT_SOURCE_DIR}/third_party" "${_lorica_root}")
string(REPLACE ";" "|" _lorica_excluded "${_lorica_excluded}")
add_test(NAME lorica_applied_compile_commands_test
         COMMAND ${CMAKE_COMMAND}
             -DMODE=${_lorica_mode}
             "-DREASON=${_lorica_reason}"
             -DCOMPILE_COMMANDS=${CMAKE_BINARY_DIR}/compile_commands.json
             -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
             -DBINARY_DIR=${PROJECT_BINARY_DIR}
             "-DEXCLUDE_DIRS=${_lorica_excluded}"
             -DLORICA_INCLUDE_DIR=${_lorica_include_dir}
             "-DUNSAFE_API_HEADER=${_lorica_unsafe_expected}"
             -DCFP_FLAG=${_lorica_cfp_flag}
             "-DFORTIFY_C=${_lorica_cc_fortify_C}"
             "-DFORTIFY_CXX=${_lorica_cc_fortify_CXX}"
             "-DGLFW_SOURCE_DIR=${_lorica_glfw_dir}"
             -P ${CMAKE_CURRENT_SOURCE_DIR}/check_compile_commands.cmake)
set_tests_properties(lorica_applied_compile_commands_test PROPERTIES
    TIMEOUT 120 SKIP_REGULAR_EXPRESSION "${_lorica_skip}")

# --- 3. the executables (python3 only) ------------------------------------------------------
set(PICTOR_LORICA_VERIFY_BINARY "${_lorica_root}/tools/verify_binary.py")
find_program(PICTOR_PYTHON3_EXECUTABLE NAMES python3 python DOC "Python 3 for Lorica's verify_binary.py")
if(NOT PICTOR_LORICA_HARDENING)
    message(STATUS "Pictor: lorica_applied_binary_*_test not registered: Lorica does not harden this toolchain")
elseif(NOT PICTOR_PYTHON3_EXECUTABLE)
    message(STATUS "Pictor: python3 not found — lorica_applied_binary_*_test (verify_binary.py) is not registered")
elseif(NOT EXISTS "${PICTOR_LORICA_VERIFY_BINARY}")
    message(STATUS "Pictor: ${PICTOR_LORICA_VERIFY_BINARY} not found — lorica_applied_binary_*_test is not registered")
else()
    set(PICTOR_LORICA_READELF_ARGS "")
    if(NOT WIN32)
        find_program(PICTOR_READELF_EXECUTABLE NAMES readelf DOC "readelf for Lorica's verify_binary.py")
        if(PICTOR_READELF_EXECUTABLE)
            set(PICTOR_LORICA_READELF_ARGS --readelf "${PICTOR_READELF_EXECUTABLE}")
        endif()
    endif()
    if(WIN32 OR PICTOR_READELF_EXECUTABLE)
        # Pictor's executables are known only at the end of the top directory (demos before
        # tests/, visus_migrate after it). The probe above is checked too.
        # Normal variables of this directory are not visible there.
        set_property(GLOBAL PROPERTY PICTOR_LORICA_VERIFY_COMMAND
                     "${PICTOR_PYTHON3_EXECUTABLE}" "${PICTOR_LORICA_VERIFY_BINARY}" ${PICTOR_LORICA_READELF_ARGS})
        cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}" CALL _pictor_lorica_register_binary_tests)
    else()
        message(STATUS "Pictor: readelf not found — lorica_applied_binary_*_test is not registered")
    endif()
endif()

function(_pictor_lorica_register_binary_tests)
    get_property(verify GLOBAL PROPERTY PICTOR_LORICA_VERIFY_COMMAND)
    pictor_own_compiled_targets(targets)
    set(registered "")
    foreach(target IN LISTS targets)
        get_target_property(kind "${target}" TYPE)
        get_target_property(dir "${target}" SOURCE_DIR)
        get_target_property(excluded "${target}" EXCLUDE_FROM_ALL)
        set(tests_dir "${PROJECT_SOURCE_DIR}/tests")
        cmake_path(IS_PREFIX tests_dir "${dir}" NORMALIZE in_tests)
        if(NOT kind STREQUAL "EXECUTABLE" OR excluded)
            continue()
        endif()
        if(in_tests AND NOT target STREQUAL "lorica_applied_tu_test")
            continue()
        endif()
        string(REGEX REPLACE "_test$" "" name "${target}")
        add_test(NAME lorica_applied_binary_${name}_test
                 COMMAND ${verify} $<TARGET_FILE:${target}>)
        set_tests_properties(lorica_applied_binary_${name}_test PROPERTIES TIMEOUT 60)
        list(APPEND registered "${target}")
    endforeach()
    message(STATUS "Pictor: verify_binary.py tests for ${registered}")
endfunction()
