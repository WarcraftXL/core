// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The Vulkan renderer inside the runtime (Vulkan.cpp); its table for the recompiled client is
// wxl/core/gx_vk.h.

#pragma once

#include "runtime/Settings.hpp"

#include <cstdint>

namespace wxl::vk
{
    namespace gxfront
    {
        class Front;
    }
    namespace device
    {
        class Context;
    }
    namespace resources
    {
        class Allocator;
    }
    namespace present
    {
        class Display;
    }
}

namespace wxl::core::gx
{
    enum class Outcome
    {
        not_asked, ///< no [gx] api = vulkan, or the client's adapter asked while it made its device
        missed,    ///< [gx] api = vulkan, but the client made a device of an API not taken to Vulkan (logged)
    };

    /**
     * @brief The client's graphics device is up (ConsoleDevice.Initialize After). Both clients ask
     *        for the Vulkan device while they make theirs, through their adapter; `[gx] api =
     *        vulkan` with a device made without it (OpenGL) is said here, once.
     * @param why  the reason when it was missed, when given
     */
    Outcome DeviceReady(const settings::Settings& settings, char* why = nullptr, uint32_t why_size = 0);

    /// Whether the client renders through the runtime's Vulkan device.
    bool VulkanActive();

    /// The engine enters (true) or leaves its world's pass (CGWorldFrame::RenderWorld): the draws
    /// in between are the world's, which the Vulkan device does not draw yet.
    void WorldPass(bool inside);
    bool InWorldPass();

    /// The Vulkan device and its open frame, for what draws into the frame beside the engine (Dear ImGui).
    struct VulkanFrame
    {
        wxl::vk::device::Context*     context = nullptr;
        wxl::vk::resources::Allocator* allocator = nullptr;
        wxl::vk::present::Display*    display = nullptr;
        uint64_t                      generation = 0; ///< grows each time the device is made again
        uint32_t                      frames = 0;     ///< frames in flight
    };

    /// The device and its frame, when a frame is open; false otherwise.
    bool VulkanOpenFrame(VulkanFrame& out);

    /// The renderer behind the client's device, while the client renders through it; null otherwise.
    wxl::vk::gxfront::Front* VulkanFront();

    /// Whether the renderer keeps each draw as a batch a mod may draw again (wxl.render's batches,
    /// wxl.highlight's): asked again at each scene and when what wants them changes.
    void UpdateBatchKeeping();
}
