# The Vulkan renderer's lower modules built natively, for tests on the machine's own driver (on a
# machine without a GPU, Mesa's lavapipe). The modules are the same rules WarcraftXL.dll is built
# from (cmake/wxl_vk.cmake); nothing of the runtime, the engine or Windows is in them.
# Copyright (c) 2026 WarcraftXL. MIT (see LICENSE).
#
#   cmake -S core/tests/vk -B build/vk -G Ninja && cmake --build build/vk && ctest --test-dir build/vk
#
# With Mesa's lavapipe alone (any other driver left out): VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json

cmake_minimum_required(VERSION 3.21)
project(wxl-vk-tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/wxl_vk.cmake")

add_executable(wxl-vk-tests vk_tests.cpp)
target_link_libraries(wxl-vk-tests PRIVATE wxl-vk)

enable_testing()
add_test(NAME vk COMMAND wxl-vk-tests)
set_tests_properties(vk PROPERTIES SKIP_RETURN_CODE 77)
