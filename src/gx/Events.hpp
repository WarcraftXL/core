// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// What the runtime does as a graphics device's resources go, whichever device it is: the D3D9
// device's release (the seam Gx.ReleaseD3dResources) and the Vulkan device's own losses, resizes
// and release (gx/Vulkan.cpp). Bodies in seams/Gx.cpp.

#pragma once

namespace wxl::core::gx
{
    /**
     * @brief Raises SwapchainLost with the size about to go away; with @p device_objects, the
     *        runtime's own objects on the device (Dear ImGui's) go first, to be made again at
     *        their next use.
     */
    void ResourcesLost(bool device_objects);
}
