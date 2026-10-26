# The Vulkan renderer's modules (src/gx/vk/, namespace wxl::vk), one static library each, in the
# order they may depend on each other: device; resources; shaders; pipelines; frame; present; draw
# (the engine's draws in Vulkan); engine (the engine's device read through the SDK's engine
# layouts, into draws); imgui. None knows the runtime or Windows: they build for Windows inside
# WarcraftXL.dll (x86 and x64) and natively for Linux in the test tree of tests/vk/, from these
# same rules. The libraries they stand on are vendored in deps/, each with its licence and VERSION:
# Vulkan-Headers, volk (the loader, found at run time), VMA (memory), SPIRV-Headers (what the
# renderer writes its own shaders against) and glslang (cmake/wxl_glsl.cmake: the renderer's GLSL
# compiled when it is built). Dear ImGui and its Vulkan backend come from the SDK's deps/imgui; the
# engine layouts from the SDK's engine/include (WXL_SDK_DIR).
# Copyright (c) 2026 WarcraftXL. MIT (see LICENSE).
#
# Needs C and C++ enabled. Defines wxl-vk, which links every module. The imgui module is defined
# when WXL_VK_IMGUI_TARGET names a target that carries Dear ImGui (its headers and sources) and
# WXL_VK_IMGUI_DIR its source directory: the runtime's wxl::imgui, or the test tree's own.

set(WXL_VK_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

# Our own code at a high warning level; the vendored code at the toolset's default.
function(_wxl_vk_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

# --- volk, over Vulkan-Headers -----------------------------------------------------------------
add_library(wxl-vk-volk STATIC "${WXL_VK_ROOT}/deps/volk/volk.c")
target_include_directories(wxl-vk-volk SYSTEM PUBLIC
    "${WXL_VK_ROOT}/deps/volk" "${WXL_VK_ROOT}/deps/vulkan-headers/include")
if(WIN32)
    target_compile_definitions(wxl-vk-volk PUBLIC VK_USE_PLATFORM_WIN32_KHR WIN32_LEAN_AND_MEAN NOMINMAX)
else()
    target_link_libraries(wxl-vk-volk PUBLIC ${CMAKE_DL_LIBS})
endif()

# --- device: the loader, the instance, the physical device, the device, its loss -----------------
add_library(wxl-vk-device STATIC
    "${WXL_VK_ROOT}/src/gx/vk/Vk.cpp"
    "${WXL_VK_ROOT}/src/gx/vk/device/Floor.cpp"
    "${WXL_VK_ROOT}/src/gx/vk/device/Device.cpp")
target_include_directories(wxl-vk-device PUBLIC "${WXL_VK_ROOT}/src")
target_link_libraries(wxl-vk-device PUBLIC wxl-vk-volk)
target_compile_features(wxl-vk-device PUBLIC cxx_std_20)
_wxl_vk_warnings(wxl-vk-device)

# --- resources: VMA, buffers, images ---------------------------------------------------------------
add_library(wxl-vk-vma STATIC "${WXL_VK_ROOT}/src/gx/vk/resources/Vma.cpp")
target_include_directories(wxl-vk-vma SYSTEM PUBLIC "${WXL_VK_ROOT}/deps/vma/include")
target_link_libraries(wxl-vk-vma PUBLIC wxl-vk-device)
add_library(wxl-vk-resources STATIC
    "${WXL_VK_ROOT}/src/gx/vk/resources/Resources.cpp"
    "${WXL_VK_ROOT}/src/gx/vk/resources/Ring.cpp"
    "${WXL_VK_ROOT}/src/gx/vk/resources/Bindless.cpp")
target_link_libraries(wxl-vk-resources PUBLIC wxl-vk-device PRIVATE wxl-vk-vma)
_wxl_vk_warnings(wxl-vk-resources)

# --- SPIRV-Headers: what the renderer writes SPIR-V against ----------------------------------------
add_library(wxl-vk-spirv-headers INTERFACE)
target_include_directories(wxl-vk-spirv-headers SYSTEM INTERFACE "${WXL_VK_ROOT}/deps/spirv-headers/include")

# --- shaders: the SPIR-V writer and the renderer's own shaders, made at run time --------------------
add_library(wxl-vk-shaders STATIC
    "${WXL_VK_ROOT}/src/gx/vk/shaders/Spirv.cpp"
    "${WXL_VK_ROOT}/src/gx/vk/shaders/Own.cpp")
target_link_libraries(wxl-vk-shaders PUBLIC wxl-vk-device wxl-vk-spirv-headers)
_wxl_vk_warnings(wxl-vk-shaders)

# --- pipelines: the renderer's programs (GLSL compiled at build time) and the pipelines of the engine's draws
include("${CMAKE_CURRENT_LIST_DIR}/wxl_glsl.cmake")
set(WXL_VK_SPIRV_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/vk_spirv")
wxl_vk_glsl(wxl-vk-programs "${WXL_VK_SPIRV_DIR}"
    ff_vert=ff.vert
    ui_vert=ui.vert
    ui_stereo_vert=ui.vert:WXL_STEREO
    ff_frag=ff.frag
    ui_frag=ui.frag
    desaturate_frag=desaturate.frag)
add_library(wxl-vk-pipelines STATIC "${WXL_VK_ROOT}/src/gx/vk/pipelines/Pipelines.cpp")
target_include_directories(wxl-vk-pipelines PRIVATE "${WXL_VK_SPIRV_DIR}")
target_link_libraries(wxl-vk-pipelines PUBLIC wxl-vk-device)
add_dependencies(wxl-vk-pipelines wxl-vk-programs)
_wxl_vk_warnings(wxl-vk-pipelines)

# --- frame: the timeline, bounded waits, the frames in flight ---------------------------------------
add_library(wxl-vk-frame STATIC "${WXL_VK_ROOT}/src/gx/vk/frame/Frame.cpp")
target_link_libraries(wxl-vk-frame PUBLIC wxl-vk-device)
_wxl_vk_warnings(wxl-vk-frame)

# --- present: surfaces, the swapchain, the present pass, a frame from start to shown ---------------
add_library(wxl-vk-present STATIC
    "${WXL_VK_ROOT}/src/gx/vk/present/Present.cpp"
    "${WXL_VK_ROOT}/src/gx/vk/present/Swapchain.cpp"
    "${WXL_VK_ROOT}/src/gx/vk/present/Pass.cpp"
    "${WXL_VK_ROOT}/src/gx/vk/present/Display.cpp")
target_link_libraries(wxl-vk-present PUBLIC wxl-vk-device wxl-vk-resources wxl-vk-frame wxl-vk-shaders)
_wxl_vk_warnings(wxl-vk-present)

# --- draw: the engine's textures, pools and draws on the device -------------------------------------
add_library(wxl-vk-draw STATIC
    "${WXL_VK_ROOT}/src/gx/vk/draw/Formats.cpp"
    "${WXL_VK_ROOT}/src/gx/vk/draw/Draw.cpp")
target_link_libraries(wxl-vk-draw PUBLIC wxl-vk-device wxl-vk-resources wxl-vk-pipelines wxl-vk-present)
_wxl_vk_warnings(wxl-vk-draw)

# --- engine: the engine's device read through the SDK's engine layouts, into draws --------------------
if(NOT WXL_VK_ENGINE_INCLUDE)
    set(WXL_VK_ENGINE_INCLUDE "${WXL_SDK_DIR}/engine/include")
endif()
add_library(wxl-vk-engine STATIC "${WXL_VK_ROOT}/src/gx/vk/gxfront/Engine.cpp")
target_include_directories(wxl-vk-engine PUBLIC "${WXL_VK_ROOT}/include")
target_include_directories(wxl-vk-engine SYSTEM PUBLIC "${WXL_VK_ENGINE_INCLUDE}")
target_link_libraries(wxl-vk-engine PUBLIC wxl-vk-draw)
_wxl_vk_warnings(wxl-vk-engine)

add_library(wxl-vk INTERFACE)
target_link_libraries(wxl-vk INTERFACE wxl-vk-device wxl-vk-resources wxl-vk-shaders wxl-vk-pipelines wxl-vk-frame wxl-vk-present
    wxl-vk-draw wxl-vk-engine)

# --- capture: a frame of the D3D9 device recorded by the client (wxl/core/gx_capture.h), read back
# and drawn again through the renderer -------------------------------------------------------------
add_library(wxl-vk-capture STATIC "${WXL_VK_ROOT}/src/gx/capture/Capture.cpp" "${WXL_VK_ROOT}/src/gx/capture/Replay.cpp")
target_include_directories(wxl-vk-capture PUBLIC "${WXL_VK_ROOT}/src" "${WXL_VK_ROOT}/include")
target_compile_features(wxl-vk-capture PUBLIC cxx_std_20)
target_link_libraries(wxl-vk-capture PUBLIC wxl-vk-engine)
_wxl_vk_warnings(wxl-vk-capture)
target_link_libraries(wxl-vk INTERFACE wxl-vk-capture)

# --- imgui: Dear ImGui's Vulkan backend over the renderer's device -----------------------------------
if(WXL_VK_IMGUI_TARGET AND WXL_VK_IMGUI_DIR)
    add_library(wxl-vk-imgui STATIC
        "${WXL_VK_ROOT}/src/gx/vk/imgui/ImGuiVk.cpp"
        "${WXL_VK_IMGUI_DIR}/backends/imgui_impl_vulkan.cpp")
    target_include_directories(wxl-vk-imgui PRIVATE "${WXL_VK_IMGUI_DIR}")
    # The backend calls Vulkan through volk's entry points, as the renderer does.
    target_compile_definitions(wxl-vk-imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK)
    target_link_libraries(wxl-vk-imgui PUBLIC wxl-vk-device wxl-vk-resources ${WXL_VK_IMGUI_TARGET})
    target_link_libraries(wxl-vk INTERFACE wxl-vk-imgui)
endif()
