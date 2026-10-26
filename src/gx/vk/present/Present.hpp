// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The window side of the renderer: what the instance needs to present and which queue family can
// present to a window. The surface, the swapchain, the scene-to-window pass and the window states
// come here with the first frame shown (step V1). On Windows it is Win32's; built elsewhere (the
// native tests) the renderer is headless and this answers that nothing presents.

#pragma once

#include "gx/vk/Vk.hpp"

#include <cstdint>

namespace wxl::vk::present
{
    /// The instance extensions a window needs, @p count of them; none when headless.
    const char* const* InstanceExtensions(uint32_t& count);

    /// Whether @p family of @p physical can present to a window of this platform.
    bool Presents(VkInstance instance, VkPhysicalDevice physical, uint32_t family);

    /// Whether this build can present at all (false on a headless build).
    bool Available();
}
