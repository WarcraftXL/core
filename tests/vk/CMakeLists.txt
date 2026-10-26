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

add_executable(wxl-vk-tests vk_tests.cpp present_tests.cpp capture_tests.cpp)
target_link_libraries(wxl-vk-tests PRIVATE wxl-vk)

enable_testing()
add_test(NAME vk COMMAND wxl-vk-tests)
set_tests_properties(vk PROPERTIES SKIP_RETURN_CODE 77)
