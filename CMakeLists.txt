# SPDX-License-Identifier: GPL-3.0-or-later
# WarcraftXL: the runtime the 3.3.5a client (build 12340, 32-bit) loads, and the patcher that makes it load it.

cmake_minimum_required(VERSION 3.20)
project(WarcraftXL LANGUAGES CXX C) # C: MinHook

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(WxlDefaults)
include(WxlDeploy)

# The client is a 32-bit process: a 64-bit configure builds nothing.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(WARNING "x64 configure builds nothing here; configure -A Win32.")
    return()
endif()

# The SDK: the installed package, or a working copy built alongside (WXL_SDK_DIR).
set(WXL_SDK_DIR "" CACHE PATH "wxl-sdk working copy to build with instead of the installed package")
if(WXL_SDK_DIR)
    add_subdirectory("${WXL_SDK_DIR}" wxl-sdk)
else()
    find_package(WarcraftXL 1.3 CONFIG REQUIRED)
endif()

option(WXL_TESTS "Build the core's tests" ON)

add_subdirectory(deps)
add_subdirectory(src)
if(WXL_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
