# Sanitizer builds for the headless test suite (GCC / Clang). Off by default.
#
#   -DPICTOR_SANITIZE=ON          AddressSanitizer + UndefinedBehaviorSanitizer
#   -DPICTOR_SANITIZE_THREAD=ON   ThreadSanitizer (job dispatcher / file watch threads)
#
# Applied to every Pictor-owned compiled target (the same set Lorica hardens), so the
# library and the tests are instrumented together. The two options are exclusive.
# Use a Debug or RelWithDebInfo build type for readable reports.
include_guard(GLOBAL)

option(PICTOR_SANITIZE        "Build Pictor with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(PICTOR_SANITIZE_THREAD "Build Pictor with ThreadSanitizer" OFF)

if(PICTOR_SANITIZE AND PICTOR_SANITIZE_THREAD)
    message(FATAL_ERROR "Pictor: PICTOR_SANITIZE and PICTOR_SANITIZE_THREAD cannot be combined")
endif()
if((PICTOR_SANITIZE OR PICTOR_SANITIZE_THREAD) AND NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
    message(FATAL_ERROR "Pictor: sanitizer builds need GCC or Clang (not ${CMAKE_CXX_COMPILER_ID})")
endif()

function(pictor_sanitize_all)
    if(PICTOR_SANITIZE)
        set(flags -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
        set(link -fsanitize=address,undefined)
    elseif(PICTOR_SANITIZE_THREAD)
        set(flags -fsanitize=thread -fno-omit-frame-pointer)
        set(link -fsanitize=thread)
    else()
        return()
    endif()
    pictor_own_compiled_targets(targets)
    foreach(target IN LISTS targets)
        target_compile_options("${target}" PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:${flags}>")
        get_target_property(kind "${target}" TYPE)
        if(kind MATCHES "^(EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY)$")
            target_link_options("${target}" PRIVATE ${link})
        endif()
    endforeach()
    message(STATUS "Pictor: sanitizer flags on Pictor targets: ${flags}")
endfunction()
