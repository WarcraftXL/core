# SPDX-License-Identifier: GPL-3.0-or-later
# Project-wide settings: C++20, the static runtime, the common definitions, the deploy folder.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Static CRT: the DLL lives in the player's Wow.exe, which carries no VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# mingw-w64: the MSVC keywords the code uses (__try, __declspec, conventions on pointers), and the C++
# runtime linked in statically.
if(MINGW)
    add_compile_options(-fms-extensions)
    add_link_options(-static)
endif()

set(WXL_DEFS WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

# Every binary at the build root (build/<Config>/ under Visual Studio).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

set(CLIENT_PATH "" CACHE PATH "Client folder the built binaries are copied into after each build")
