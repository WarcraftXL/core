// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The window side of the renderer, Win32's.

#include "gx/vk/present/Present.hpp"

namespace wxl::vk::present
{
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
#else
    const char* const* InstanceExtensions(uint32_t& count)
    {
        count = 0;
        return nullptr;
    }

    bool Presents(VkInstance, VkPhysicalDevice, uint32_t) { return false; }

    bool Available() { return false; }
#endif
}
