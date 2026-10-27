# wxl-core: the WarcraftXL runtime, one source tree built twice. WarcraftXL.dll for the stock
# Wow.exe 3.3.5a (32-bit, pulled into the client by an import the patcher adds) and WarcraftXL.dll
# for the recompiled client (64-bit, linked by the client and started through WarcraftXLInit).
# The mod host comes from the SDK checkout next door and is built here once per architecture.
# The patcher, MinHook and the two mods under mods/ carry client addresses and exist on x86 only.
# Copyright (c) 2026 WarcraftXL. MIT (see LICENSE).
#
# One CMake tree is one architecture, so this file plays two roles depending on WXL_CORE_ARCH:
#
#   - Unset (`cmake -S . -B build`): a superbuild. cmake/superbuild.cmake configures and builds
#     this directory once per architecture as a child project, each with the matching Visual
#     Studio platform, and installs what each produces into one folder:
#
#       build/dist/bin/x86/WarcraftXL.dll
#       build/dist/bin/x86/wxl-core-patcher.exe
#       build/dist/bin/x64/WarcraftXL.dll
#       build/dist/Mods/<id>/{mod.json,bin/x86/<name>.dll}
#       build/dist/Mods/wxl.lua/{mod.json,LICENSE-LuaJIT.txt,bin/x86/wxl-lua.dll,bin/x64/wxl-lua.dll}
#
#   - win-x86 or win-x64 (a child, or a tree configured by hand with the matching -A Win32 or
#     -A x64): the real targets of that one architecture, in build/<Config>/.

cmake_minimum_required(VERSION 3.21)
project(wxl-core VERSION 0.2.0 LANGUAGES NONE)

set(WXL_CORE_ARCH "" CACHE STRING
    "Architecture of this build tree (win-x86 or win-x64). Set by the superbuild; empty builds every target.")
option(WXL_CORE_MODS "Build the mods under mods/, each for the architectures it lists" ON)
set(WXL_CORE_MODS_DIR "" CACHE PATH
    "Where the mods of mods/ land, as <dir>/<id>/{mod.json,bin/<arch>/}. Empty: <this tree>/<Config>/Mods")
option(WXL_CORE_LUA "Build wxl.lua, the runtime of the Lua mods, from the SDK and ship it in Mods/" ON)
option(WXL_PROFILE "Carry the Tracy profiler client, on demand, and publish wxl.profile; WarcraftXL.ini's [profile] starts it; OFF leaves no trace of it" ON)

set(WXL_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sdk" CACHE PATH "Path to a checkout of WarcraftXL/sdk")
if(NOT EXISTS "${WXL_SDK_DIR}/include/wxl/abi.h")
    message(FATAL_ERROR "WXL_SDK_DIR (${WXL_SDK_DIR}) is not a WarcraftXL/sdk checkout: "
        "include/wxl/abi.h is missing. Pass -DWXL_SDK_DIR=<path to WarcraftXL/sdk>.")
endif()

if(NOT WXL_CORE_ARCH)
    include(cmake/superbuild.cmake)
    return()
endif()

# --- One architecture from here on ---------------------------------------------------------------
if(WXL_CORE_ARCH STREQUAL "win-x86")
    set(WXL_CORE_X86 ON)
    set(WXL_CORE_ARCH_DIR x86)
    set(WXL_CORE_POINTER_SIZE 4)
elseif(WXL_CORE_ARCH STREQUAL "win-x64")
    set(WXL_CORE_X86 OFF)
    set(WXL_CORE_ARCH_DIR x64)
    set(WXL_CORE_POINTER_SIZE 8)
else()
    message(FATAL_ERROR "WXL_CORE_ARCH is '${WXL_CORE_ARCH}'; it must be win-x86 or win-x64.")
endif()

# Static CRT: the DLL loads inside the player's Wow.exe, which has no reason to carry the toolset's
# VC++ redistributable. A dynamic CRT turns a missing redist into a loader failure at process start
# (0xc0000142) with no log at all. Set before any language is enabled so every target, the SDK's
# host library included, matches.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

enable_language(CXX)
enable_language(C) # volk on both architectures, MinHook on x86

if(NOT CMAKE_SIZEOF_VOID_P EQUAL WXL_CORE_POINTER_SIZE)
    math(EXPR have "${CMAKE_SIZEOF_VOID_P} * 8")
    math(EXPR want "${WXL_CORE_POINTER_SIZE} * 8")
    message(FATAL_ERROR "this tree is WXL_CORE_ARCH=${WXL_CORE_ARCH} (${want}-bit) but the compiler "
        "targets ${have}-bit. With a Visual Studio generator pass the matching platform "
        "(-A Win32 for win-x86, -A x64 for win-x64); with another generator, configure from a "
        "developer prompt of the right architecture.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# /EHs rather than /EHsc: a mod's function reached through a C function pointer may throw, and
# the service that called it catches that at the boundary. With 'c' the compiler would assume
# it cannot. The SDK's host library makes the same choice for the same reason.
string(REPLACE "/EHsc" "/EHs" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

# Every binary lands at <this tree>/<Config>/ whatever folder declares it; the current binary
# directory rather than the top one, so that a client may add this directory as its own
# subdirectory and build the DLL it links.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

# --- The SDK: the contract, the shared mod host and the shared runtime half of wxl.object -------
set(WXL_SDK_TESTS OFF CACHE BOOL "" FORCE)
# The host's dispatch and handler zones follow the runtime's choice.
set(WXL_HOST_PROFILE ${WXL_PROFILE} CACHE BOOL "" FORCE)
add_subdirectory("${WXL_SDK_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/sdk" EXCLUDE_FROM_ALL)

# A single integer a mod can read out of wxl_runtime_info.version: major*10000 + minor*100 + patch.
math(EXPR WXL_CORE_VERSION
    "${PROJECT_VERSION_MAJOR} * 10000 + ${PROJECT_VERSION_MINOR} * 100 + ${PROJECT_VERSION_PATCH}")

set(WXL_CORE_DEFS WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

# Symbols for the shipped build: a crash in a player's log is an address, and without a PDB
# beside the binary there is nothing to turn it into a function and a line.
#
# /Zi makes the compiler emit debug information; /DEBUG makes the linker write the PDB. /DEBUG
# also turns the linker's own defaults for /OPT:REF and /OPT:ICF off, which would leave
# unreferenced code and duplicate COMDATs in the image that ships, so both are named back
# explicitly: named that way they reproduce the image the linker builds on its own defaults.
#
# On the targets that also ask for whole-program optimisation, /DEBUG still costs the image a
# little: the link-time code generator pads .text differently when it has debug information to
# carry, 16 bytes of it on x86 and 448 on x64, which shifts most functions by 16 or 32 bytes
# and so rewrites every relative call. The link map holds the same 2734 .text symbols either
# way -- nothing is kept that /OPT:REF would have dropped. wxl-core-patcher, which has no
# whole-program optimisation, keeps its .text size exactly.
function(wxl_core_release_symbols target)
    target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(${target} PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
endfunction()

# --- WarcraftXL.dll --------------------------------------------------------------------------------
# What both architectures share: the log, the start-up, the mod host behind it, the bodies of
# the seams (one file per domain of include/wxl/seams/), the leaf reads of wxl.object
# (src/objects/) and wxl.script, which reaches the client's Lua
# through the binding like everything else. Each architecture adds its entry point, its
# attachment to the client and its realisation of the client binding below: memory reads at
# measured offsets on x86 (BindingX86.cpp), the accessors the client handed over on x64
# (BindingTable.cpp).
# The domain bodies and the object reads are globbed with CONFIGURE_DEPENDS: adding a domain means
# adding its .def and its .cpp, and CMake picks the file up on the next build without this list
# being edited. Table.cpp is swept up with them: it is the table as data, which both read.
file(GLOB WXL_SEAM_BODIES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/seams/*.cpp")
list(FILTER WXL_SEAM_BODIES EXCLUDE REGEX "/AttachX86\.cpp$")
# x86 only: hand-written naked thunks for the seams whose arguments arrive in registers.
list(FILTER WXL_SEAM_BODIES EXCLUDE REGEX "/RegparmX86\.cpp$")
file(GLOB WXL_OBJECT_READS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/objects/*.cpp")

# Dear ImGui's Win32 and Direct3D 9 backends, from the SDK's copy (deps/imgui/backends): the
# runtime draws the mods' windows with the game's own device and feeds them the window's input.
set(WXL_IMGUI_BACKENDS
    "${WXL_IMGUI_DIR}/backends/imgui_impl_dx9.cpp"
    "${WXL_IMGUI_DIR}/backends/imgui_impl_win32.cpp")

# The commits the build comes from, read at every build into a header only Session.cpp includes:
# core's, the SDK's, and the client's when core is built inside a client's tree.
set(WXL_BUILD_IDS_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(WXL_BUILD_IDS_CLIENT "")
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(WXL_BUILD_IDS_CLIENT "${CMAKE_SOURCE_DIR}")
endif()
add_custom_target(wxl-core-build-ids
    COMMAND "${CMAKE_COMMAND}" "-DOUT=${WXL_BUILD_IDS_DIR}/wxl_build_ids.h"
            "-DCORE=${CMAKE_CURRENT_SOURCE_DIR}" "-DSDK=${WXL_SDK_DIR}" "-DCLIENT=${WXL_BUILD_IDS_CLIENT}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_ids.cmake"
    BYPRODUCTS "${WXL_BUILD_IDS_DIR}/wxl_build_ids.h"
    COMMENT "Reading the commits WarcraftXL.dll is built from"
    VERBATIM)

# --- The Vulkan renderer (src/gx/vk/, wxl::vk) --------------------------------------------------
# Its modules, one static library each, with volk, VMA, Vulkan-Headers and SPIRV-Headers from deps/
# (cmake/wxl_vk.cmake, which the native test tree of tests/vk/ builds from too). The runtime's side
# of it is src/gx/: WarcraftXL.ini's [gx] api, the table the recompiled client's CGxDeviceVk calls
# (include/wxl/core/gx_vk.h) and the device made on the stock client.
set(WXL_VK_IMGUI_TARGET wxl::imgui)
set(WXL_VK_IMGUI_DIR "${WXL_IMGUI_DIR}")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/wxl_vk.cmake")
set(WXL_CORE_GX_SOURCES
    src/gx/Memory.cpp
    src/gx/Pacing.cpp
    src/gx/Select.cpp
    src/gx/Vulkan.cpp
    src/gx/vk/gxfront/Front.cpp)

# --- wxl.highlight's Direct3D 9 passes (src/gx/d3d9/, the stock client) ---------------------------
# Its programs are HLSL compiled when the runtime is built, by wxl-core-d3d9-shaders with the
# system's d3dcompiler_47 (Wine's when built from Linux: the tool then runs under
# CMAKE_CROSSCOMPILING_EMULATOR), and held to what shaders.txt allows each one to read. The bytecode
# lands in generated/d3d9_shaders.inc, the disassembly beside it. The recompiled client draws
# highlights through its own renderer, so x64 builds none of this; wxl.draw's programs, below, are
# built on both.
set(WXL_CORE_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(WXL_D3D9_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/gx/d3d9/shaders")
# The tool runs where the build runs Windows programs: on Windows, or through the emulator.
set(WXL_D3D9_TOOL_RUNS ON)
if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
    set(WXL_D3D9_TOOL_RUNS OFF)
endif()
if(WXL_CORE_X86 AND NOT WXL_D3D9_TOOL_RUNS)
    message(FATAL_ERROR "The x86 runtime compiles its Direct3D 9 programs with a Windows tool when it is built: "
        "building it from another system needs CMAKE_CROSSCOMPILING_EMULATOR (Wine), e.g. "
        "export CMAKE_CROSSCOMPILING_EMULATOR=$HOME/scratch/build/tools/wine-run.sh")
endif()
if(WXL_D3D9_TOOL_RUNS)
    add_executable(wxl-core-d3d9-shaders tools/shaders/compile_d3d9.cpp)
    target_compile_options(wxl-core-d3d9-shaders PRIVATE /W4 /permissive-)
    target_compile_definitions(wxl-core-d3d9-shaders PRIVATE ${WXL_CORE_DEFS})
endif()
if(WXL_CORE_X86)
    set(WXL_D3D9_SHADERS "${WXL_CORE_GENERATED_DIR}/d3d9_shaders.inc")
    add_custom_command(OUTPUT "${WXL_D3D9_SHADERS}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${WXL_CORE_GENERATED_DIR}"
        COMMAND wxl-core-d3d9-shaders "${WXL_D3D9_SHADER_DIR}/shaders.txt" "${WXL_D3D9_SHADERS}"
        DEPENDS wxl-core-d3d9-shaders "${WXL_D3D9_SHADER_DIR}/shaders.txt" "${WXL_D3D9_SHADER_DIR}/highlight.hlsl"
        COMMENT "Compiling the runtime's Direct3D 9 programs"
        VERBATIM)
    list(APPEND WXL_CORE_GX_SOURCES src/gx/d3d9/Highlight.cpp "${WXL_D3D9_SHADERS}")
    # The stock client's device drawn by the Vulkan renderer: the slots of include/wxl/gx/slots.def.
    list(APPEND WXL_CORE_GX_SOURCES src/gx/x86/Adapter.cpp)
endif()

# --- wxl.draw's Direct3D 9 programs (src/gx/d3d9/Shapes.cpp, both architectures) ----------------
# The stock client and the recompiled one on Direct3D 9 draw shapes with them. Compiled as
# wxl.highlight's are where the tool runs; an x64 tree cross-built without the emulator takes the
# copy kept in src/gx/d3d9/shaders/generated/, which a build that ran the tool compares with its own.
set(WXL_D3D9_DRAW_SHADERS "${WXL_CORE_GENERATED_DIR}/d3d9_draw_shaders.inc")
set(WXL_D3D9_DRAW_KEPT "${WXL_D3D9_SHADER_DIR}/generated/d3d9_draw_shaders.inc")
if(WXL_D3D9_TOOL_RUNS)
    add_custom_command(OUTPUT "${WXL_D3D9_DRAW_SHADERS}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${WXL_CORE_GENERATED_DIR}"
        COMMAND wxl-core-d3d9-shaders "${WXL_D3D9_SHADER_DIR}/draw.txt" "${WXL_D3D9_DRAW_SHADERS}"
        COMMAND "${CMAKE_COMMAND}" "-DMADE=${WXL_D3D9_DRAW_SHADERS}" "-DKEPT=${WXL_D3D9_DRAW_KEPT}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/compare_kept.cmake"
        DEPENDS wxl-core-d3d9-shaders "${WXL_D3D9_SHADER_DIR}/draw.txt" "${WXL_D3D9_SHADER_DIR}/draw.hlsl"
        COMMENT "Compiling wxl.draw's Direct3D 9 programs"
        VERBATIM)
else()
    message(STATUS "wxl.draw's Direct3D 9 programs: the kept copy (no CMAKE_CROSSCOMPILING_EMULATOR to run the compiler)")
    configure_file("${WXL_D3D9_DRAW_KEPT}" "${WXL_D3D9_DRAW_SHADERS}" COPYONLY)
endif()
list(APPEND WXL_CORE_GX_SOURCES src/gx/d3d9/Shapes.cpp "${WXL_D3D9_DRAW_SHADERS}")

# The launch's log (its settings file, its header and its end, the crash report, the client's fatal
# errors) and the code the client accepts.
set(WXL_CORE_SESSION_SOURCES
    src/runtime/Code.cpp
    src/runtime/Session.cpp
    src/runtime/Settings.cpp
    src/runtime/Crash.cpp
    src/runtime/Stamp.cpp)

# --- Pictures made into textures (wxl.texture) ----------------------------------------------------
# nanosvg (zlib, deps/nanosvg) rasterizes SVG, stb_image (MIT or public domain, deps/stb) decodes
# PNG; the resampler, the mips and the BLP2 writer are ours. One static library, so the tests below
# check the very code the runtime links.
add_library(wxl-core-imaging STATIC src/imaging/Svg.cpp src/imaging/Image.cpp)
target_include_directories(wxl-core-imaging PUBLIC src PRIVATE deps/nanosvg deps/stb)
# wxl.net's zlib streams: stb_image's inflater and stb_image_write's deflater, in one source file
# the runtime and the harness compile.
set_source_files_properties(src/net/Zlib.cpp PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/deps/stb")

add_library(wxl-core SHARED
    src/runtime/Core.cpp
    src/runtime/Listening.cpp
    src/points/Points.cpp
    ${WXL_CORE_SESSION_SOURCES}
    src/runtime/Stages.cpp
    src/runtime/Frame.cpp
    src/common/Log.cpp
    src/services/ScriptService.cpp
    src/services/DataService.cpp
    src/services/Assets.cpp
    src/services/StorageService.cpp
    src/services/Hashes.cpp
    src/services/Reload.cpp
    src/services/RelayService.cpp
    src/services/FootprintService.cpp
    src/services/ActionService.cpp
    src/services/GxService.cpp
    src/services/EngineService.cpp
    src/services/M2Service.cpp
    src/services/M2FileService.cpp
    src/services/CameraService.cpp
    src/services/InputService.cpp
    src/services/WorldService.cpp
    src/services/PoseService.cpp
    src/services/HighlightService.cpp
    src/services/RenderService.cpp
    src/services/SelectionCircleService.cpp
    src/services/TextureService.cpp
    src/services/CVarService.cpp
    src/services/TerrainService.cpp
    src/services/ShaderCode.cpp
    src/services/TerrainLayers.cpp
    src/services/DrawService.cpp
    src/services/AnimationService.cpp
    src/services/NetService.cpp
    src/net/Zlib.cpp
    src/services/ImGuiService.cpp
    src/tuning/Animate.cpp
    src/tuning/Drops.cpp
    ${WXL_IMGUI_BACKENDS}
    ${WXL_CORE_GX_SOURCES}
    ${WXL_SEAM_BODIES}
    ${WXL_OBJECT_READS})
set_target_properties(wxl-core PROPERTIES OUTPUT_NAME "WarcraftXL" PREFIX "")
# Whole-program optimisation in the shipped build only. The runtime is spread thin on purpose --
# the binding, the log gate, the stage check and Core::Host() are one-line functions in their own
# translation units, called from the hottest paths there are -- so without this the linker cannot
# see through any of them. Release only: it costs link time and makes a debug build harder to
# follow, and neither matters to what ships.
set_property(TARGET wxl-core PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
wxl_core_release_symbols(wxl-core)
target_include_directories(wxl-core PRIVATE src include "${WXL_BUILD_IDS_DIR}" "${WXL_CORE_GENERATED_DIR}")
add_dependencies(wxl-core wxl-core-build-ids)
target_link_libraries(wxl-core PRIVATE wxl::host wxl::runtime wxl::engine wxl::imgui wxl-vk wxl-core-imaging bcrypt synchronization)
target_compile_definitions(wxl-core PRIVATE
    ${WXL_CORE_DEFS} WXL_CORE_VERSION=${WXL_CORE_VERSION} WXL_CORE_BUILD)

# --- The seam harness --------------------------------------------------------------------------
# The seam bodies are the same code on both architectures, so they can be checked without a
# client: this links the bodies themselves, the object reads and the table-filled client binding
# against a real mod host, binds the client binding to a fake world, and drives each seam's
# Before and After. It supplies its own Host() in place of Core.cpp and the attachments, which
# carry an architecture's entry point. Built for both, run by hand or by ctest.
add_executable(wxl-core-seam-harness
    tests/seam_harness.cpp
    src/runtime/Listening.cpp
    src/points/Points.cpp
    ${WXL_CORE_SESSION_SOURCES}
    src/common/Log.cpp
    src/runtime/Stages.cpp
    src/runtime/Frame.cpp
    src/binding/BindingTable.cpp
    src/services/ScriptService.cpp
    src/services/DataService.cpp
    src/services/Assets.cpp
    src/services/StorageService.cpp
    src/services/Hashes.cpp
    src/services/Reload.cpp
    src/services/RelayService.cpp
    src/services/FootprintService.cpp
    src/services/ActionService.cpp
    src/services/GxService.cpp
    src/services/EngineService.cpp
    src/services/M2Service.cpp
    src/services/M2FileService.cpp
    src/services/CameraService.cpp
    src/services/InputService.cpp
    src/services/WorldService.cpp
    src/services/PoseService.cpp
    src/services/HighlightService.cpp
    src/services/RenderService.cpp
    src/services/SelectionCircleService.cpp
    src/services/TextureService.cpp
    src/services/CVarService.cpp
    src/services/TerrainService.cpp
    src/services/ShaderCode.cpp
    src/services/TerrainLayers.cpp
    src/services/DrawService.cpp
    src/services/AnimationService.cpp
    src/services/NetService.cpp
    src/net/Zlib.cpp
    src/services/ImGuiService.cpp
    src/tuning/Animate.cpp
    src/tuning/Drops.cpp
    ${WXL_IMGUI_BACKENDS}
    ${WXL_CORE_GX_SOURCES}
    ${WXL_SEAM_BODIES}
    ${WXL_OBJECT_READS})
# The same build as the DLL, so --bench measures what ships rather than something near it.
set_property(TARGET wxl-core-seam-harness PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
target_include_directories(wxl-core-seam-harness PRIVATE src include "${WXL_BUILD_IDS_DIR}" "${WXL_CORE_GENERATED_DIR}")
add_dependencies(wxl-core-seam-harness wxl-core-build-ids)
target_link_libraries(wxl-core-seam-harness PRIVATE wxl::host wxl::runtime wxl::engine wxl::imgui wxl-vk wxl-core-imaging bcrypt synchronization)
target_compile_definitions(wxl-core-seam-harness PRIVATE
    ${WXL_CORE_DEFS} WXL_CORE_VERSION=${WXL_CORE_VERSION} WXL_CORE_BUILD)

# --- The log ---------------------------------------------------------------------------------------
# The session files (a launch's log renamed for the next, the oldest pruned), the levels by tag,
# the settings file and the graphics API it selects, against a scratch folder.
add_executable(wxl-core-log-tests
    tests/log_tests.cpp
    src/common/Log.cpp
    src/runtime/Settings.cpp
    src/gx/Select.cpp)
target_include_directories(wxl-core-log-tests PRIVATE src include "${WXL_SDK_DIR}/include")
target_compile_definitions(wxl-core-log-tests PRIVATE ${WXL_CORE_DEFS})

# --- Tracy (BSD-3-Clause, deps/tracy, its own license file) -----------------------------------------
# The client half of the profiler, compiled once into the DLL: mods reach it through wxl.profile,
# the recompiled client through WarcraftXLZoneBegin/End, so no other binary carries a second one.
# On demand: nothing is sent, and a zone costs a load, until a viewer connects. Started by the
# runtime (profile::Start) rather than at load, which would run under the loader lock. No crash
# handler: the mod host's guard owns faults. No system tracing, sampling or call stacks: they need
# rights or symbol files a player does not have, and Proton serves none of them. Whether it starts,
# its port and whether it listens on 127.0.0.1 alone are WarcraftXL.ini's [profile], set at run time
# through the variables Tracy reads; the address it would announce itself to is fixed at compile
# time (255.255.255.255, the whole network), so it does not announce itself at all: a viewer
# connects to the address it is given.
if(WXL_PROFILE)
    add_library(wxl-tracy STATIC deps/tracy/public/TracyClient.cpp)
    target_include_directories(wxl-tracy SYSTEM PUBLIC deps/tracy/public)
    target_compile_definitions(wxl-tracy PUBLIC
        TRACY_ENABLE TRACY_ON_DEMAND TRACY_DELAYED_INIT TRACY_MANUAL_LIFETIME TRACY_NO_CRASH_HANDLER
        TRACY_NO_SYSTEM_TRACING TRACY_NO_SAMPLING TRACY_NO_CONTEXT_SWITCH TRACY_NO_VSYNC_CAPTURE
        TRACY_NO_CALLSTACK TRACY_NO_CODE_TRANSFER TRACY_NO_VERIFY TRACY_NO_FRAME_IMAGE TRACY_NO_BROADCAST
        ${WXL_CORE_DEFS})
    target_link_libraries(wxl-tracy PUBLIC ws2_32)

    foreach(target wxl-core wxl-core-seam-harness)
        target_sources(${target} PRIVATE src/profile/Profile.cpp)
        target_link_libraries(${target} PRIVATE wxl-tracy)
    endforeach()
    # PUBLIC: the client's sources read it too, to compile their zones (wxl/core/profile.h).
    target_compile_definitions(wxl-core PUBLIC WXL_CORE_PROFILE=1)
    target_compile_definitions(wxl-core-seam-harness PRIVATE WXL_CORE_PROFILE=1)
endif()

# --- The Vulkan renderer's lower modules on the machine's driver --------------------------------------
# The same program as the native tree of tests/vk/: the device floor and choice, a device made and
# refused, VMA, the timeline, a clear read back. Skipped (77) on a machine without a Vulkan 1.3 device.
add_executable(wxl-core-vk-tests tests/vk/vk_tests.cpp tests/vk/present_tests.cpp tests/vk/capture_tests.cpp
    tests/vk/draw_tests.cpp tests/vk/spirv_tests.cpp tests/vk/translate_tests.cpp tests/vk/v4_tests.cpp
    tests/vk/parity_tests.cpp tests/vk/render_tests.cpp tests/vk/v7_tests.cpp tests/vk/shapes_tests.cpp tests/vk/v5_tests.cpp)
target_link_libraries(wxl-core-vk-tests PRIVATE wxl-vk)
# wxl.modern-adt's height blend, its rewritten terrain programs drawn on the driver.
target_sources(wxl-core-vk-tests PRIVATE mods/wxl.modern-adt/src/shader/Ps3.cpp)
target_include_directories(wxl-core-vk-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/mods/wxl.modern-adt/src")
include(tests/vk/programs.cmake)
wxl_vk_add_test_programs(wxl-core-vk-tests)
target_compile_options(wxl-core-vk-tests PRIVATE /W4 /permissive-)
target_compile_definitions(wxl-core-vk-tests PRIVATE _CRT_SECURE_NO_WARNINGS)

enable_testing()
add_test(NAME seams COMMAND wxl-core-seam-harness)
add_test(NAME log COMMAND wxl-core-log-tests "${CMAKE_CURRENT_BINARY_DIR}/log-scratch")
add_test(NAME vk COMMAND wxl-core-vk-tests)

# The rasterizer, the decoder, the resampler and the BLP writer, pixel by pixel.
add_executable(wxl-core-imaging-tests tests/imaging_tests.cpp)
target_link_libraries(wxl-core-imaging-tests PRIVATE wxl-core-imaging)
add_test(NAME imaging COMMAND wxl-core-imaging-tests)

# wxl.modern-adt's reading layer, store and features, on ADTs made up byte by byte.
set(WXL_MODERN_ADT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/mods/wxl.modern-adt")
add_executable(wxl-core-modern-adt-tests "${WXL_MODERN_ADT_DIR}/tests/modern_adt_tests.cpp" "${WXL_MODERN_ADT_DIR}/src/Options.cpp"
    "${WXL_MODERN_ADT_DIR}/src/adt/Reader.cpp" "${WXL_MODERN_ADT_DIR}/src/store/Store.cpp" "${WXL_MODERN_ADT_DIR}/src/features/TextureScale.cpp"
    "${WXL_MODERN_ADT_DIR}/src/features/HeightBlend.cpp" "${WXL_MODERN_ADT_DIR}/src/features/ExtraLayers.cpp"
    "${WXL_MODERN_ADT_DIR}/src/shader/Ps3.cpp")
target_include_directories(wxl-core-modern-adt-tests PRIVATE "${WXL_MODERN_ADT_DIR}/src" "${WXL_SDK_DIR}/include")
# A chunk of a real DawnlightWorld root ADT (WorldGen's, five layers), compiled into the tests.
file(READ "${WXL_MODERN_ADT_DIR}/tests/data/DawnlightWorld_38_19_chunk_14_4.bin" _root_chunk HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _root_chunk "${_root_chunk}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/modern_adt_root_chunk.inc" "${_root_chunk}\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${WXL_MODERN_ADT_DIR}/tests/data/DawnlightWorld_38_19_chunk_14_4.bin")
target_include_directories(wxl-core-modern-adt-tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_features(wxl-core-modern-adt-tests PRIVATE cxx_std_20)
add_test(NAME modern-adt COMMAND wxl-core-modern-adt-tests)
set_tests_properties(vk PROPERTIES SKIP_RETURN_CODE 77)

install(TARGETS wxl-core RUNTIME DESTINATION "bin/${WXL_CORE_ARCH_DIR}" COMPONENT core)
install(FILES "$<TARGET_PDB_FILE:wxl-core>" DESTINATION "bin/${WXL_CORE_ARCH_DIR}"
    COMPONENT core OPTIONAL)

if(WXL_CORE_X86)
    # --- MinHook (BSD-2-Clause, deps/minhook, its own license file) --------------------------------
    # Declared here rather than through its own CMakeLists, which predates the CMake in use.
    # Third-party code: built at the toolset's default warning level, not ours.
    add_library(minhook STATIC
        deps/minhook/src/buffer.c
        deps/minhook/src/hook.c
        deps/minhook/src/trampoline.c
        deps/minhook/src/hde/hde32.c)
    target_include_directories(minhook PUBLIC deps/minhook/include)
    target_include_directories(minhook PRIVATE deps/minhook/src deps/minhook/src/hde)

    # --- SafetyHook (BSL-1.0, deps/safetyhook) and Zydis (MIT, deps/zydis) -----------------------
    # The detours of wxl.point: SafetyHook writes and removes them, armed while a point has
    # handlers, and its mid-function hooks reach a point's inlined copies; Zydis, its decoder, also
    # checks a function's first bytes before anything is written. Only the decoder of Zydis is kept.
    # SafetyHook is C++23 (std::expected): it and the one file that includes it, src/hook/Safety.cpp,
    # build here as their own library, behind the plain interface of src/hook/Safety.hpp.
    file(GLOB WXL_ZYDIS_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/deps/zydis/src/*.c")
    add_library(wxl-zydis STATIC ${WXL_ZYDIS_SOURCES} deps/zydis/zycore/src/Zycore.c)
    target_include_directories(wxl-zydis PUBLIC deps/zydis/include deps/zydis/zycore/include
                               PRIVATE deps/zydis/src)
    target_compile_definitions(wxl-zydis PUBLIC ZYDIS_STATIC_BUILD ZYCORE_STATIC_BUILD
                               ZYDIS_DISABLE_ENCODER ZYDIS_DISABLE_FORMATTER)
    add_library(wxl-safetyhook STATIC
        deps/safetyhook/src/allocator.cpp
        deps/safetyhook/src/inline_hook.cpp
        deps/safetyhook/src/mid_hook.cpp
        deps/safetyhook/src/os.windows.cpp
        deps/safetyhook/src/utility.cpp
        src/hook/Safety.cpp)
    set_target_properties(wxl-safetyhook PROPERTIES CXX_STANDARD 23)
    target_include_directories(wxl-safetyhook PUBLIC deps/safetyhook/include PRIVATE src)
    target_compile_definitions(wxl-safetyhook PRIVATE ${WXL_CORE_DEFS})
    target_link_libraries(wxl-safetyhook PUBLIC wxl-zydis)

    # The stock client is a shipped binary: the DLL reaches it by an import and an entry
    # trampoline the patcher adds, detours at resolved addresses, and byte patches. Every file
    # here holds an address, reads the image's headers, or serves a service built on those.
    # The recompiled parent bodies of the de-inlined points, one file per WXL_POINT_X86_PARENT row
    # of include/wxl/points/x86.def (tools/extract/extract.py --deinlined writes them).
    set(WXL_CORE_DEINLINED
        src/deinlined/CM2Shared_FinishLoading.gen.cpp
        src/deinlined/CM2Shared_FinishLoadingLowPrioritySequence_uint32_t_uint8_t_uint32_t.gen.cpp)
    target_sources(wxl-core PRIVATE
        src/runtime/Main.cpp
        src/runtime/AttachmentX86.cpp
        src/runtime/Image.cpp
        src/binding/BindingX86.cpp
        src/seams/AttachX86.cpp
        src/seams/RegparmX86.cpp
        src/hook/Engine.cpp
        src/hook/Points.cpp
        src/points/PointsX86.cpp
        ${WXL_CORE_DEINLINED}
        src/services/HookService.cpp
        src/services/PatchService.cpp)
    target_link_libraries(wxl-core PRIVATE minhook wxl-safetyhook)
    # The harness drives the register thunks against stand-ins with the client's conventions, and
    # arms points on stand-ins in its own image through the real hook engine.
    target_sources(wxl-core-seam-harness PRIVATE
        src/seams/RegparmX86.cpp
        src/runtime/Image.cpp
        src/hook/Engine.cpp
        src/points/PointsX86.cpp
        ${WXL_CORE_DEINLINED})
    target_link_libraries(wxl-core-seam-harness PRIVATE minhook wxl-safetyhook)

    # --- The recompiled bodies against Wow.exe ------------------------------------------------------
    # src/deinlined/ run against a stock Wow.exe 12340's own code, laid at its own base in this
    # executable's .bss: linked low and fixed, so that the reservation covers 0x00400000 and up.
    # Given no Wow.exe (WXL_WOW_EXE, at configure or run time), it says so and passes.
    add_executable(wxl-core-stock-tests
        tests/stock_tests.cpp
        src/points/Points.cpp
        src/points/PointsX86.cpp
        ${WXL_CORE_DEINLINED}
        src/hook/Engine.cpp
        src/runtime/Image.cpp
        src/common/Log.cpp)
    target_include_directories(wxl-core-stock-tests PRIVATE src include)
    target_link_libraries(wxl-core-stock-tests PRIVATE wxl::host wxl::runtime wxl::engine minhook wxl-safetyhook)
    target_compile_definitions(wxl-core-stock-tests PRIVATE ${WXL_CORE_DEFS} WXL_CORE_BUILD)
    # Sections on 64 KB: the free memory after the image then starts where VirtualAlloc can reserve,
    # which SafetyHook's search for a trampoline near Wow.exe's functions needs.
    target_link_options(wxl-core-stock-tests PRIVATE /BASE:0x10000 /DYNAMICBASE:NO /FIXED /ALIGN:0x10000)
    target_compile_options(wxl-core-stock-tests PRIVATE /W4 /permissive-)
    set(WXL_WOW_EXE "$ENV{WXL_WOW_EXE}" CACHE FILEPATH "A stock Wow.exe 12340, for wxl-core-stock-tests")
    add_test(NAME stock COMMAND wxl-core-stock-tests ${WXL_WOW_EXE})

    # --- wxl-core-patcher.exe ----------------------------------------------------------------------
    add_executable(wxl-core-patcher
        src/patcher/Main.cpp
        src/patcher/PeImage.cpp
        src/patcher/PatchScript.cpp
        src/common/Log.cpp)
    set_target_properties(wxl-core-patcher PROPERTIES OUTPUT_NAME "wxl-core-patcher")
    wxl_core_release_symbols(wxl-core-patcher)
    target_include_directories(wxl-core-patcher PRIVATE src)
    target_compile_definitions(wxl-core-patcher PRIVATE ${WXL_CORE_DEFS})

    install(TARGETS wxl-core-patcher RUNTIME DESTINATION bin/x86 COMPONENT core)
    install(FILES "$<TARGET_PDB_FILE:wxl-core-patcher>" DESTINATION bin/x86 COMPONENT core OPTIONAL)

    set(WXL_CORE_WARNED_TARGETS wxl-core wxl-core-patcher)
else()
    # The recompiled client links the DLL and calls WarcraftXLInit. Nothing is detoured and no
    # address exists: the seams are calls the client's own source makes, and the client is read
    # through the accessors it handed over.
    target_sources(wxl-core PRIVATE
        src/runtime/Init.cpp
        src/runtime/AttachmentX64.cpp
        src/binding/BindingTable.cpp
        src/points/PointsX64.cpp)
    # The harness hands it a point table of its own, as the client does.
    target_sources(wxl-core-seam-harness PRIVATE src/points/PointsX64.cpp)

    # The import library, for the client to link; the headers it needs are under include/.
    install(TARGETS wxl-core ARCHIVE DESTINATION "lib/${WXL_CORE_ARCH_DIR}" COMPONENT core)

    set(WXL_CORE_WARNED_TARGETS wxl-core)
endif()

list(APPEND WXL_CORE_WARNED_TARGETS wxl-core-seam-harness wxl-core-log-tests)
foreach(target IN LISTS WXL_CORE_WARNED_TARGETS)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
endforeach()

# --- wxl.lua, on both architectures: the Lua mods run on either runtime ----------------------------
if(WXL_CORE_LUA)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/wxl_lua.cmake")
endif()

# --- The mods this runtime ships -------------------------------------------------------------------
if(WXL_CORE_MODS)
    add_subdirectory(mods)
endif()
