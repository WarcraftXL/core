// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The Vulkan API as the renderer reaches it: volk's entry points (deps/volk), loaded from the
// system's loader at run time, so that nothing links vulkan-1.dll and a machine without one starts
// on D3D9. The device's functions are loaded straight from the device (volkLoadDevice): one
// renderer per process, no dispatch through the loader's trampolines. On Windows the build defines
// VK_USE_PLATFORM_WIN32_KHR for every file of the renderer, which volk.h reads.

#pragma once

#include <volk.h>

#include <cstdint>

namespace wxl::vk
{
    /// The lowest Vulkan version the renderer runs on, for the loader and for the device.
    constexpr uint32_t kFloorApi = VK_API_VERSION_1_3;

    /// "1.3.290" for a packed version.
    void FormatVersion(uint32_t version, char* out, uint32_t size);

    /// The name of a result, "VK_ERROR_DEVICE_LOST", or "VkResult <n>" (in a buffer of the calling thread) for one
    /// it does not know.
    const char* ResultName(VkResult result);
}
