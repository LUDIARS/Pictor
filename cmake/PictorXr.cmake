# 任意モジュール: XR (両眼カメラ・差分レンダリング・OpenXR backend)。
#
#   PICTOR_ENABLE_XR      両眼カメラ / カリング / 再投影 / 差分描画の計画。 外部依存なし。
#   PICTOR_ENABLE_OPENXR  OpenXR ランタイム (Meta Quest Link 等) への接続。 Vulkan が必要。
#
# 外したモジュールは実装・外部ライブラリ・shader 生成・専用 demo を一切要求しない
# (spec/architecture/framework-boundaries.md PC-RULE-002)。 要求されたのに前提が
# 欠けている構成は、 黙って無効化せず FATAL_ERROR で止める。
option(PICTOR_ENABLE_XR     "Build the optional XR module (stereo camera, delta rendering)" ON)
option(PICTOR_ENABLE_OPENXR "Build the OpenXR backend of the XR module (needs Vulkan)" OFF)

set(PICTOR_OPENXR_SDK_TAG "release-1.1.63" CACHE STRING
    "OpenXR-SDK git tag fetched when no installed OpenXR package is found")

if(PICTOR_ENABLE_OPENXR AND NOT PICTOR_ENABLE_XR)
    message(FATAL_ERROR "PICTOR_ENABLE_OPENXR=ON には PICTOR_ENABLE_XR=ON が必要")
endif()
if(PICTOR_ENABLE_OPENXR AND NOT Vulkan_FOUND)
    message(FATAL_ERROR "PICTOR_ENABLE_OPENXR=ON には Vulkan が必要 (OpenXR backend は Vulkan 専用)")
endif()

if(NOT PICTOR_ENABLE_XR)
    return()
endif()

add_library(pictor_xr STATIC
    src/xr/stereo_camera.cpp
    src/xr/stereo_frustum.cpp
    src/xr/stereo_presets.cpp
    src/xr/reprojection.cpp
    src/xr/delta_render_planner.cpp
    # Vulkan が無いビルドでは中身が空になる (ソース側で PICTOR_HAS_VULKAN を見る)。
    src/xr/reprojection_pass.cpp)
add_library(Pictor::xr ALIAS pictor_xr)
target_link_libraries(pictor_xr PUBLIC pictor)
target_compile_definitions(pictor_xr PUBLIC PICTOR_HAS_XR=1)
if(MSVC)
    target_compile_options(pictor_xr PRIVATE /W4 /utf-8)
endif()

if(Vulkan_FOUND)
    pictor_effect_shaders(pictor_xr
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/xr/reproject_warp.vert;${CMAKE_CURRENT_SOURCE_DIR}/shaders/xr/reproject_warp.frag"
        "${CMAKE_CURRENT_BINARY_DIR}/shaders/xr")
endif()

if(PICTOR_ENABLE_OPENXR)
    find_package(OpenXR CONFIG QUIET)
    if(NOT OpenXR_FOUND)
        include(FetchContent)
        set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
        set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
        set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            openxr_sdk
            GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
            GIT_TAG        ${PICTOR_OPENXR_SDK_TAG}
            GIT_SHALLOW    TRUE)
        FetchContent_MakeAvailable(openxr_sdk)
    endif()
    if(NOT TARGET OpenXR::openxr_loader)
        message(FATAL_ERROR "PICTOR_ENABLE_OPENXR=ON だが OpenXR::openxr_loader が得られない")
    endif()

    target_sources(pictor_xr PRIVATE
        src/xr/openxr/openxr_runtime.cpp
        src/xr/openxr/openxr_session.cpp
        src/xr/openxr/openxr_swapchain_surface.cpp
        src/xr/openxr/openxr_input.cpp)
    # OpenXR の型は公開ヘッダへ出さないので、 consumer へはリンクだけを伝える。
    target_link_libraries(pictor_xr PRIVATE OpenXR::openxr_loader)
    target_compile_definitions(pictor_xr PUBLIC PICTOR_HAS_OPENXR=1)
endif()
