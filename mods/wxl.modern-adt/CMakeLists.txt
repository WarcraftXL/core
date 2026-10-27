cmake_minimum_required(VERSION 3.21)

# No language here: wxl_add_mod enables C++ in the per-architecture child build.
project(wxl-modern-adt VERSION 1.0.0 LANGUAGES NONE)

# Standalone: the SDK checkout next to core. From core's own tree wxl_add_mod is already defined.
if(NOT COMMAND wxl_add_mod)
    set(WXL_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../sdk" CACHE PATH "Path to the WarcraftXL SDK")
    include("${WXL_SDK_DIR}/cmake/wxl_add_mod.cmake")
endif()

# Both clients: it reads ADT bytes and answers wxl.terrain, which both runtimes serve alike.
wxl_add_mod(modern-adt
    SOURCES src/mod.cpp src/Options.cpp src/adt/Reader.cpp src/store/Store.cpp src/features/TextureScale.cpp
    MANIFEST mod.json
    TARGETS win-x86 win-x64)

if(TARGET modern-adt)
    target_include_directories(modern-adt PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()

# From core's tree the binary, the manifest and the options also go to build/<Config>/Mods/.
if(TARGET modern-adt AND COMMAND wxl_core_mod)
    wxl_core_mod(modern-adt wxl.modern-adt)
    if(WXL_CORE_MODS_DIR)
        set(_modern_adt_dir "${WXL_CORE_MODS_DIR}/wxl.modern-adt")
    else()
        set(_modern_adt_dir "${CMAKE_BINARY_DIR}/$<CONFIG>/Mods/wxl.modern-adt")
    endif()
    add_custom_command(TARGET modern-adt POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/modern-adt.ini" "${_modern_adt_dir}/modern-adt.ini"
        COMMENT "wxl.modern-adt: modern-adt.ini -> Mods/wxl.modern-adt")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/modern-adt.ini" DESTINATION "Mods/wxl.modern-adt" COMPONENT core)
endif()
