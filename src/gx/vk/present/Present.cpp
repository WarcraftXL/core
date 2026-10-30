// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// Surfaces: Win32's on Windows, the headless one anywhere its driver has it.

#if defined(_WIN32)
#include <windows.h>
#endif

#include "gx/vk/present/Present.hpp"

#include <cstdio>

namespace wxl::vk::present
{
    const char* const* HeadlessExtensions(uint32_t& count)
    {
        static const char* const kExtensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
        count = 2;
        return kExtensions;
    }

    VkSurfaceKHR CreateHeadlessSurface(VkInstance instance, char* why, uint32_t why_size)
    {
        if (!vkCreateHeadlessSurfaceEXT)
        {
            std::snprintf(why, why_size, "the instance has no " VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
            return VK_NULL_HANDLE;
        }
        VkHeadlessSurfaceCreateInfoEXT info{};
        info.sType            = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
        VkSurfaceKHR   surface = VK_NULL_HANDLE;
        const VkResult result  = vkCreateHeadlessSurfaceEXT(instance, &info, nullptr, &surface);
        if (result != VK_SUCCESS)
        {
            std::snprintf(why, why_size, "vkCreateHeadlessSurfaceEXT failed: %s", ResultName(result));
            return VK_NULL_HANDLE;
        }
        return surface;
    }

#if defined(_WIN32)
    const char* const* InstanceExtensions(uint32_t& count)
    {
        static const char* const kExtensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
        count = 2;
        return kExtensions;
    }

    // Answered without a window: Win32 asks the family, not a surface, so the device is chosen
    // before the engine has made its window.
    bool Presents(VkInstance, VkPhysicalDevice physical, uint32_t family)
    {
        return vkGetPhysicalDeviceWin32PresentationSupportKHR &&
               vkGetPhysicalDeviceWin32PresentationSupportKHR(physical, family) == VK_TRUE;
    }

    bool Available() { return true; }

    VkSurfaceKHR CreateWindowSurface(VkInstance instance, void* window, char* why, uint32_t why_size)
    {
        if (!vkCreateWin32SurfaceKHR || !window)
        {
            std::snprintf(why, why_size, !window ? "there is no window to present to" : "the instance has no " VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
            return VK_NULL_HANDLE;
        }
        VkWin32SurfaceCreateInfoKHR info{};
        info.sType            = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        info.hinstance        = GetModuleHandleW(nullptr);
        info.hwnd             = static_cast<HWND>(window);
        VkSurfaceKHR   surface = VK_NULL_HANDLE;
        const VkResult result  = vkCreateWin32SurfaceKHR(instance, &info, nullptr, &surface);
        if (result != VK_SUCCESS)
        {
            std::snprintf(why, why_size, "vkCreateWin32SurfaceKHR failed: %s", ResultName(result));
            return VK_NULL_HANDLE;
        }
        return surface;
    }
#else
    const char* const* InstanceExtensions(uint32_t& count)
    {
        count = 0;
        return nullptr;
    }

    bool Presents(VkInstance, VkPhysicalDevice, uint32_t) { return false; }

    bool Available() { return false; }

    VkSurfaceKHR CreateWindowSurface(VkInstance, void*, char* why, uint32_t why_size)
    {
        std::snprintf(why, why_size, "this build has no window system");
        return VK_NULL_HANDLE;
    }
#endif
}
