# WebGL2 backend (Emscripten) and the Web mesh module package (SPEC-PC-WEB-MESH).
# Included from the root CMakeLists.txt: once early for Emscripten toolchains (which build
# only these web targets — the core library needs Vulkan), once in the normal flow when
# PICTOR_BUILD_WEBGL is set.

# ============================================================
# WebGL2 backend (Emscripten)
# ============================================================
if(PICTOR_BUILD_WEBGL OR EMSCRIPTEN)
    add_library(pictor_webgl STATIC
        src/core/types.cpp
        src/webgl/webgl_context.cpp
        src/webgl/webgl_shader.cpp
        src/webgl/webgl_buffer.cpp
        src/webgl/webgl_renderer.cpp
        # Web mesh module (SPEC-PC-WEB-MESH)
        src/webgl/web_mesh_input.cpp
        src/webgl/web_mesh_renderer.cpp
    )

    target_include_directories(pictor_webgl PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )

    target_compile_definitions(pictor_webgl PUBLIC PICTOR_HAS_WEBGL=1)

    if(EMSCRIPTEN)
        target_compile_options(pictor_webgl PRIVATE -sUSE_WEBGL2=1)
        target_link_options(pictor_webgl PUBLIC -sUSE_WEBGL2=1 -sFULL_ES3=1)
    endif()

    # WebGL demo (Emscripten only)
    if(PICTOR_BUILD_DEMO AND EMSCRIPTEN)
        add_executable(pictor_webgl_demo demo/webgl/main.cpp)
        target_link_libraries(pictor_webgl_demo PRIVATE pictor_webgl)
        target_link_options(pictor_webgl_demo PRIVATE
            -sUSE_WEBGL2=1
            -sFULL_ES3=1
            -sALLOW_MEMORY_GROWTH=1
            -sEXPORTED_RUNTIME_METHODS=['ccall','cwrap']
            --shell-file ${CMAKE_SOURCE_DIR}/demo/webgl/shell.html
        )
        set_target_properties(pictor_webgl_demo PROPERTIES
            SUFFIX ".html"
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/webgl
        )
        # Copy shaders
        add_custom_command(TARGET pictor_webgl_demo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_SOURCE_DIR}/shaders/webgl
                ${CMAKE_BINARY_DIR}/webgl/shaders
        )
        message(STATUS "Pictor: WebGL2 demo enabled (Emscripten)")
    endif()

    # Web mesh module package (SPEC-PC-WEB-MESH): an ES module + wasm with the C ABI of
    # src/webgl/web_mesh_exports.cpp, staged with the JavaScript face from web-mesh/ into
    # <build>/web-mesh/ so that directory can be packed and pinned by consumers.
    if(EMSCRIPTEN)
        add_executable(pictor_web_mesh src/webgl/web_mesh_exports.cpp)
        target_link_libraries(pictor_web_mesh PRIVATE pictor_webgl)
        target_link_options(pictor_web_mesh PRIVATE
            -sUSE_WEBGL2=1
            -sFULL_ES3=1
            -sENVIRONMENT=web
            -sMODULARIZE=1
            -sEXPORT_ES6=1
            -sEXPORT_NAME=createPictorWebMeshModule
            -sALLOW_MEMORY_GROWTH=1
            -sFILESYSTEM=0
            "-sEXPORTED_FUNCTIONS=['_malloc','_free']"
            "-sEXPORTED_RUNTIME_METHODS=['HEAPU8','HEAPU32','HEAPF32']"
        )
        set_target_properties(pictor_web_mesh PROPERTIES
            SUFFIX ".mjs"
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/web-mesh
        )
        add_custom_command(TARGET pictor_web_mesh POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_SOURCE_DIR}/web-mesh/index.js
                ${CMAKE_SOURCE_DIR}/web-mesh/index.d.ts
                ${CMAKE_SOURCE_DIR}/web-mesh/heap.js
                ${CMAKE_SOURCE_DIR}/web-mesh/lighting.js
                ${CMAKE_SOURCE_DIR}/web-mesh/package.json
                ${CMAKE_SOURCE_DIR}/web-mesh/README.md
                ${CMAKE_BINARY_DIR}/web-mesh
        )
        message(STATUS "Pictor: web mesh module package enabled (Emscripten)")
    endif()
endif()
