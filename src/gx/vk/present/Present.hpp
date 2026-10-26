// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The window side of the renderer: what the instance needs to present, which queue family can
// present to a window, and the surface made on a window. On Windows it is Win32's. A machine with
// no window system (the native tests) presents to a headless surface (VK_EXT_headless_surface)
// when its driver has one: a real swapchain with nothing on screen.

#pragma once

#include "gx/vk/Vk.hpp"

#include <cstdint>

namespace wxl::vk::present
{
    /// The instance extensions a window needs, @p count of them; none when this build has no window.
    const char* const* InstanceExtensions(uint32_t& count);

    /// The instance extensions a headless surface needs (VK_KHR_surface, VK_EXT_headless_surface).
    const char* const* HeadlessExtensions(uint32_t& count);

    /// Whether @p family of @p physical can present to a window of this platform.
    bool Presents(VkInstance instance, VkPhysicalDevice physical, uint32_t family);

    /// Whether this build can present to a window at all (false on a build without one).
    bool Available();

    /// A surface on @p window (an HWND on Windows), or null with @p why.
    VkSurfaceKHR CreateWindowSurface(VkInstance instance, void* window, char* why, uint32_t why_size);

    /// A headless surface (the instance must have HeadlessExtensions), or null with @p why.
    VkSurfaceKHR CreateHeadlessSurface(VkInstance instance, char* why, uint32_t why_size);
}
