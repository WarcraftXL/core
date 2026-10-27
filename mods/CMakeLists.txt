# The mods core ships: each one a folder with its own CMakeLists.txt, mod.json and src/, written
# against the SDK like any other mod and buildable on its own. Built from here they land ready to
# copy in build/<Config>/Mods/<id>/{mod.json,bin/<arch>/<name>.dll} (or WXL_CORE_MODS_DIR), and
# `cmake --install` of the `core` component (what the superbuild runs) collects the same folder
# into <prefix>/Mods/<id>/.
# Copyright (c) 2026 WarcraftXL. MIT (see LICENSE).

# This tree is one architecture; wxl_add_mod then declares a real library target for a mod that
# lists it, and nothing for one that does not, instead of a per-architecture superbuild.
set(WXL_MOD_ARCH ${WXL_CORE_ARCH})
include("${WXL_SDK_DIR}/cmake/wxl_add_mod.cmake")

# Routes a mod's binary and manifest into the Mods/ layout a runtime scans. Called by the mod's
# own CMakeLists.txt, next to its target, when it is built from this tree.
#   wxl_core_mod(<target> <manifest id>)
function(wxl_core_mod target id)
    if(WXL_CORE_MODS_DIR)
        set(mod_dir "${WXL_CORE_MODS_DIR}/${id}")
    else()
        set(mod_dir "${CMAKE_BINARY_DIR}/$<CONFIG>/Mods/${id}")
    endif()
    set_target_properties(${target} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${mod_dir}/bin/${WXL_CORE_ARCH_DIR}"
        CXX_STANDARD 20)
    target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}/include")
    target_compile_definitions(${target} PRIVATE ${WXL_CORE_DEFS})
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/mod.json" "${mod_dir}/mod.json"
        COMMENT "${id}: mod.json -> Mods/${id}")
    install(TARGETS ${target} LIBRARY DESTINATION "Mods/${id}/bin/${WXL_CORE_ARCH_DIR}" COMPONENT core)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/mod.json" DESTINATION "Mods/${id}" COMPONENT core)
endfunction()

# wxl_add_mod builds a mod for the tree's architecture whatever its TARGETS, so a mod that lists one
# architecture is only added on that one.
add_subdirectory(wxl.glue-unlock)
add_subdirectory(wxl.modern-adt)
if(WXL_CORE_X86)
    add_subdirectory(wxl.named-archives)
endif()
