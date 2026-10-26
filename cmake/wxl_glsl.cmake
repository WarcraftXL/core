# The renderer's own shaders: GLSL in src/gx/vk/shaders/glsl/, compiled to SPIR-V when the runtime
# is built, by glslang (deps/glslang) built for the machine that builds, and embedded as arrays.
# Nothing is compiled at run time and nothing of glslang ships.
# Copyright (c) 2026 WarcraftXL. MIT (see LICENSE).
#
#   WXL_GLSLANG   a glslang (or glslangValidator) already built: used as it is. Empty: deps/glslang
#                 is built once in <build>/host-glslang, for this machine, with the generator of
#                 this tree and none of the cross toolchain (CMAKE_TOOLCHAIN_FILE and
#                 CMAKE_CROSSCOMPILING_EMULATOR are taken out of its environment).
#
# wxl_vk_glsl(<target> <output dir> <name>=<source>[:<DEFINE>...] ...) compiles each source for
# Vulkan 1.3 into <output dir>/<name>.h (the array `<name>`), and makes <target> a target the
# renderer's library depends on.

set(WXL_GLSLANG "" CACHE FILEPATH "A glslang executable to compile the renderer's GLSL with; empty builds deps/glslang for this machine")

if(WXL_GLSLANG)
    set(WXL_GLSLANG_EXE "${WXL_GLSLANG}")
    set(WXL_GLSLANG_TARGET "")
elseif(NOT TARGET wxl-host-glslang)
    include(ExternalProject)
    set(_wxl_glslang_bin "${CMAKE_BINARY_DIR}/host-glslang")
    set(_wxl_host_exe "")
    if(CMAKE_HOST_WIN32)
        set(_wxl_host_exe ".exe")
    endif()
    if(CMAKE_GENERATOR MATCHES "^Visual Studio")
        # A multi-configuration generator puts the tool under its configuration's folder.
        set(WXL_GLSLANG_EXE "${_wxl_glslang_bin}/StandAlone/Release/glslang${_wxl_host_exe}")
    else()
        set(WXL_GLSLANG_EXE "${_wxl_glslang_bin}/StandAlone/glslang${_wxl_host_exe}")
    endif()
    set(_wxl_glslang_generator -G "${CMAKE_GENERATOR}")
    if(CMAKE_GENERATOR MATCHES "^Visual Studio" AND CMAKE_GENERATOR_INSTANCE)
        list(APPEND _wxl_glslang_generator "-DCMAKE_GENERATOR_INSTANCE=${CMAKE_GENERATOR_INSTANCE}")
    endif()
    if(CMAKE_GENERATOR MATCHES "Ninja|Makefiles" AND CMAKE_MAKE_PROGRAM)
        list(APPEND _wxl_glslang_generator "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}")
    endif()
    ExternalProject_Add(wxl-host-glslang
        SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../deps/glslang"
        BINARY_DIR "${_wxl_glslang_bin}"
        PREFIX     "${_wxl_glslang_bin}-steps"
        CONFIGURE_COMMAND
            "${CMAKE_COMMAND}" -E env --unset=CMAKE_TOOLCHAIN_FILE --unset=CMAKE_CROSSCOMPILING_EMULATOR
                --unset=CC --unset=CXX --unset=CFLAGS --unset=CXXFLAGS
            "${CMAKE_COMMAND}" -S <SOURCE_DIR> -B <BINARY_DIR> ${_wxl_glslang_generator}
                -DCMAKE_BUILD_TYPE=Release -DENABLE_OPT=OFF -DGLSLANG_TESTS=OFF -DENABLE_HLSL=OFF
                -DENABLE_GLSLANG_BINARIES=ON -DGLSLANG_ENABLE_INSTALL=OFF -DBUILD_SHARED_LIBS=OFF
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release --target glslang-standalone
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS "${WXL_GLSLANG_EXE}"
        USES_TERMINAL_BUILD ON)
    set(WXL_GLSLANG_TARGET wxl-host-glslang)
endif()

function(wxl_vk_glsl target out_dir)
    set(outputs)
    set(glsl_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/gx/vk/shaders/glsl")
    file(GLOB includes "${glsl_dir}/*.glsl")
    foreach(spec IN LISTS ARGN)
        string(REPLACE "=" ";" parts "${spec}")
        list(GET parts 0 name)
        list(GET parts 1 rest)
        string(REPLACE ":" ";" rest "${rest}")
        list(GET rest 0 source)
        list(REMOVE_AT rest 0)
        set(defines)
        foreach(define IN LISTS rest)
            list(APPEND defines "-D${define}")
        endforeach()
        set(out "${out_dir}/${name}.h")
        add_custom_command(OUTPUT "${out}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${out_dir}"
            COMMAND "${WXL_GLSLANG_EXE}" -V --target-env vulkan1.3 ${defines} "-I${glsl_dir}" --vn "${name}"
                    -o "${out}" "${glsl_dir}/${source}"
            DEPENDS "${glsl_dir}/${source}" ${includes} ${WXL_GLSLANG_TARGET}
            COMMENT "Compiling the renderer's ${source} to SPIR-V (${name})"
            VERBATIM)
        list(APPEND outputs "${out}")
    endforeach()
    add_custom_target(${target} DEPENDS ${outputs})
endfunction()
