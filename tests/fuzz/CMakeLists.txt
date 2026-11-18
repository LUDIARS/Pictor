# Fuzz entry points for Pictor's parsers of untrusted input.
#
# Every entry builds a regression executable (fuzz_regression_main.cpp replays seeds,
# prefixes and deterministic mutations) registered with ctest, on every compiler.
# -DPICTOR_BUILD_FUZZERS=ON (Clang on Linux only, default OFF) also builds a
# libFuzzer binary per entry:
#   ./tests/fuzz/pictor_fuzz_fbx_document -max_total_time=30
option(PICTOR_BUILD_FUZZERS "Build libFuzzer binaries for Pictor's parsers (Clang only)" OFF)
if(PICTOR_BUILD_FUZZERS AND NOT (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    message(FATAL_ERROR "PICTOR_BUILD_FUZZERS=ON requires Clang on Linux (libFuzzer)")
endif()

set(PICTOR_FUZZ_ENTRIES fbx_document font_loader visus_json web_mesh_input)

foreach(_entry IN LISTS PICTOR_FUZZ_ENTRIES)
    set(_source ${CMAKE_CURRENT_SOURCE_DIR}/fuzz_${_entry}.cpp)
    set(_deps pictor)
    if(_entry STREQUAL "web_mesh_input")
        # Pure CPU contract of the Web mesh module, compiled in as the native test does.
        list(APPEND _source ${PROJECT_SOURCE_DIR}/src/webgl/web_mesh_input.cpp)
    endif()

    add_executable(pictor_fuzz_regression_${_entry} ${_source} fuzz_regression_main.cpp)
    target_link_libraries(pictor_fuzz_regression_${_entry} PRIVATE ${_deps})
    target_include_directories(pictor_fuzz_regression_${_entry} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
    if(MSVC)
        target_compile_options(pictor_fuzz_regression_${_entry} PRIVATE /utf-8)
    endif()
    add_test(NAME pictor_fuzz_regression_${_entry} COMMAND pictor_fuzz_regression_${_entry})
    set_tests_properties(pictor_fuzz_regression_${_entry} PROPERTIES TIMEOUT 120)

    if(PICTOR_BUILD_FUZZERS)
        add_executable(pictor_fuzz_${_entry} ${_source})
        target_link_libraries(pictor_fuzz_${_entry} PRIVATE ${_deps})
        target_include_directories(pictor_fuzz_${_entry} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
        target_compile_options(pictor_fuzz_${_entry} PRIVATE -fsanitize=fuzzer,address,undefined)
        target_link_options(pictor_fuzz_${_entry} PRIVATE -fsanitize=fuzzer,address,undefined)
    endif()
endforeach()
unset(_entry)
unset(_source)
unset(_deps)
