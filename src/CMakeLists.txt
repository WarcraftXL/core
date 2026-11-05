# SPDX-License-Identifier: GPL-3.0-or-later
# The 32-bit binaries: WarcraftXL.dll, injected into the client, and wxl-patcher.exe beside it.

# src/common (log, config, memory writes) is compiled into both binaries.
file(GLOB WXL_COMMON_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/common/*.cpp")
add_library(wxl_common OBJECT ${WXL_COMMON_SRC})
target_include_directories(wxl_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(wxl_common PUBLIC WarcraftXL::sdk)
target_compile_definitions(wxl_common PRIVATE ${WXL_DEFS})

# WarcraftXL.dll: every .cpp under src/ but common/ and patcher/.
file(GLOB_RECURSE WXL_CORE_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
list(FILTER WXL_CORE_SRC EXCLUDE REGEX "/src/common/|/src/patcher/")

add_library(WarcraftXL SHARED ${WXL_CORE_SRC})
set_target_properties(WarcraftXL PROPERTIES OUTPUT_NAME "WarcraftXL" PREFIX "")
target_include_directories(WarcraftXL PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/deps/minhook/include")
target_link_libraries(WarcraftXL PRIVATE WarcraftXL::game wxl_common minhook)
target_compile_definitions(WarcraftXL PRIVATE ${WXL_DEFS})
source_group(TREE "${CMAKE_SOURCE_DIR}" FILES ${WXL_CORE_SRC})

wxl_deploy(WarcraftXL WarcraftXL.dll)

add_subdirectory(patcher)
