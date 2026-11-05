# SPDX-License-Identifier: GPL-3.0-or-later
# Toolchain: the 32-bit Windows binaries from Linux with clang for mingw-w64 (GCC cannot build the SEH code).

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)
set(WXL_MINGW_TRIPLE i686-w64-mingw32)
set(WXL_MINGW_ROOT "$ENV{WXL_MINGW_ROOT}" CACHE PATH "Root of an llvm-mingw install (its bin/ holds i686-w64-mingw32-clang++)")

# llvm-mingw or MSYS2 clang from WXL_MINGW_ROOT, the PATH or ~/.local/opt/llvm-mingw; else the host
# clang retargeted onto the distribution's mingw-w64 sysroot.
set(_wxl_hints "$ENV{HOME}/.local/opt/llvm-mingw/bin")
if(WXL_MINGW_ROOT)
    list(PREPEND _wxl_hints "${WXL_MINGW_ROOT}/bin")
endif()
find_program(WXL_MINGW_CXX NAMES "${WXL_MINGW_TRIPLE}-clang++" HINTS ${_wxl_hints})
find_program(WXL_MINGW_CC NAMES "${WXL_MINGW_TRIPLE}-clang" HINTS ${_wxl_hints})

if(WXL_MINGW_CXX AND WXL_MINGW_CC)
    set(CMAKE_C_COMPILER "${WXL_MINGW_CC}")
    set(CMAKE_CXX_COMPILER "${WXL_MINGW_CXX}")
    get_filename_component(_wxl_bin "${WXL_MINGW_CXX}" DIRECTORY)
    find_program(CMAKE_RC_COMPILER NAMES "${WXL_MINGW_TRIPLE}-windres" llvm-windres HINTS "${_wxl_bin}")
else()
    find_program(_wxl_build_clangxx NAMES clang++ REQUIRED)
    find_program(_wxl_build_clang NAMES clang REQUIRED)
    set(CMAKE_C_COMPILER "${_wxl_build_clang}")
    set(CMAKE_CXX_COMPILER "${_wxl_build_clangxx}")
    set(CMAKE_C_COMPILER_TARGET "${WXL_MINGW_TRIPLE}")
    set(CMAKE_CXX_COMPILER_TARGET "${WXL_MINGW_TRIPLE}")
    if(EXISTS "/usr/${WXL_MINGW_TRIPLE}/sys-root/mingw")
        set(CMAKE_SYSROOT "/usr/${WXL_MINGW_TRIPLE}/sys-root/mingw")
    endif()
    find_program(CMAKE_RC_COMPILER NAMES "${WXL_MINGW_TRIPLE}-windres" llvm-windres)
    find_program(_wxl_lld NAMES ld.lld lld)
    if(_wxl_lld)
        set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
        set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
    endif()
endif()

# Nothing of the Linux host is a Windows dependency.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
