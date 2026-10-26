# The Vulkan renderer's modules (src/gx/vk/, namespace wxl::vk), one static library each, in the
# order they may depend on each other: device; resources; shaders; frame; present (which sees all
# of them); imgui. None knows the engine or the runtime: they build for Windows inside
# WarcraftXL.dll (x86 and x64) and natively for Linux in the test tree of tests/vk/, from these
# same rules. The libraries they stand on are vendored in deps/, each with its licence and VERSION:
# Vulkan-Headers, volk (the loader, found at run time), VMA (memory) and SPIRV-Headers (what the
# renderer writes its own shaders against). Dear ImGui and its Vulkan backend come from the SDK's
# deps/imgui.
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
add_library(wxl-vk-resources STATIC "${WXL_VK_ROOT}/src/gx/vk/resources/Resources.cpp")
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

add_library(wxl-vk INTERFACE)
target_link_libraries(wxl-vk INTERFACE wxl-vk-device wxl-vk-resources wxl-vk-shaders wxl-vk-frame wxl-vk-present)

# --- capture: a frame of the D3D9 device recorded by the client (wxl/core/gx_capture.h), read back --
add_library(wxl-vk-capture STATIC "${WXL_VK_ROOT}/src/gx/capture/Capture.cpp")
target_include_directories(wxl-vk-capture PUBLIC "${WXL_VK_ROOT}/src" "${WXL_VK_ROOT}/include")
target_compile_features(wxl-vk-capture PUBLIC cxx_std_20)
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
