cmake_minimum_required(VERSION 3.21)

# No language here: wxl_add_mod enables C++ in the per-architecture child build.
project(wxl-glue-unlock VERSION 2.0.0 LANGUAGES NONE)

# Standalone: the SDK checkout next to core. From core's own tree wxl_add_mod is already defined.
if(NOT COMMAND wxl_add_mod)
    set(WXL_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../sdk" CACHE PATH "Path to the WarcraftXL SDK")
    include("${WXL_SDK_DIR}/cmake/wxl_add_mod.cmake")
endif()

# Both clients: it answers the InterfaceSignature and InterfaceCleanupRequest events, which each
# runtime raises from its own client.
wxl_add_mod(glue-unlock
    SOURCES src/mod.cpp
    MANIFEST mod.json
    TARGETS win-x86 win-x64)

# From core's tree the binary and the manifest also go to the Mods/ folder core fills.
if(TARGET glue-unlock AND COMMAND wxl_core_mod)
    wxl_core_mod(glue-unlock wxl.glue-unlock)
endif()
