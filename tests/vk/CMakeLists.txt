# The Vulkan renderer's lower modules built natively, for tests on the machine's own driver (on a
# machine without a GPU, Mesa's lavapipe). The modules are the same rules WarcraftXL.dll is built
# from (cmake/wxl_vk.cmake); nothing of the runtime, the engine or Windows is in them. Dear ImGui
# comes from the SDK checkout beside this one (WXL_SDK_DIR), as it does for the runtime.
# Copyright (c) 2026 WarcraftXL. MIT (see LICENSE).
#
#   cmake -S core/tests/vk -B build/vk -G Ninja && cmake --build build/vk && ctest --test-dir build/vk
#
# With Mesa's lavapipe alone (any other driver left out): VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json
# The images the tests draw are written to WXL_VK_TEST_OUT (a folder) when it is set, as PPM files.

cmake_minimum_required(VERSION 3.21)
project(wxl-vk-tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(WXL_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../sdk" CACHE PATH "Path to a checkout of WarcraftXL/sdk")
set(WXL_VK_IMGUI_DIR "${WXL_SDK_DIR}/deps/imgui")
if(NOT EXISTS "${WXL_VK_IMGUI_DIR}/imgui.h")
    message(FATAL_ERROR "Dear ImGui is not at ${WXL_VK_IMGUI_DIR}: pass -DWXL_SDK_DIR=<path to WarcraftXL/sdk>.")
endif()
add_library(wxl-vk-test-imgui STATIC
    "${WXL_VK_IMGUI_DIR}/imgui.cpp"
    "${WXL_VK_IMGUI_DIR}/imgui_draw.cpp"
    "${WXL_VK_IMGUI_DIR}/imgui_tables.cpp"
    "${WXL_VK_IMGUI_DIR}/imgui_widgets.cpp")
target_include_directories(wxl-vk-test-imgui SYSTEM PUBLIC "${WXL_VK_IMGUI_DIR}")
set(WXL_VK_IMGUI_TARGET wxl-vk-test-imgui)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/wxl_vk.cmake")

# SPIRV-Tools (deps/spirv-tools), for spirv-val over every program the renderer makes: the tests
# only, never the runtime.
set(SPIRV-Headers_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../deps/spirv-headers")
set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)
set(SKIP_SPIRV_TOOLS_INSTALL ON CACHE BOOL "" FORCE)
set(SPIRV_TOOLS_BUILD_STATIC ON CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../deps/spirv-tools" spirv-tools EXCLUDE_FROM_ALL)

add_executable(wxl-vk-tests vk_tests.cpp present_tests.cpp capture_tests.cpp draw_tests.cpp spirv_tests.cpp translate_tests.cpp
    v4_tests.cpp parity_tests.cpp render_tests.cpp v7_tests.cpp)
target_link_libraries(wxl-vk-tests PRIVATE wxl-vk SPIRV-Tools-static)
include("${CMAKE_CURRENT_SOURCE_DIR}/programs.cmake")
wxl_vk_add_test_programs(wxl-vk-tests)
target_compile_definitions(wxl-vk-tests PRIVATE WXL_VK_SPIRV_VAL)

# A frame capture drawn again on this machine's driver and compared with its D3D9 back buffer:
#   wxl-vk-replay <capture folder> [<output folder>]
add_executable(wxl-vk-replay replay_tool.cpp)
target_link_libraries(wxl-vk-replay PRIVATE wxl-vk)

enable_testing()
add_test(NAME vk COMMAND wxl-vk-tests)
set_tests_properties(vk PROPERTIES SKIP_RETURN_CODE 77)

# The translator over a client's shader corpus (extracted from the user's own archives, never in a
# repository): wxl-vk-corpus <folder> [--device]. ctest runs it when WXL_VK_CORPUS names the folder.
add_executable(wxl-vk-corpus corpus_tool.cpp corpus_device.cpp)
target_link_libraries(wxl-vk-corpus PRIVATE wxl-vk SPIRV-Tools-static)
if(DEFINED ENV{WXL_VK_CORPUS})
    add_test(NAME vk-corpus COMMAND wxl-vk-corpus "$ENV{WXL_VK_CORPUS}" --device)
    set_tests_properties(vk-corpus PROPERTIES SKIP_RETURN_CODE 77)
endif()
