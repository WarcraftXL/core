// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The Vulkan renderer inside the runtime (Vulkan.cpp); its table for the recompiled client is
// wxl/core/gx_vk.h.

#pragma once

#include "runtime/Settings.hpp"

#include <cstdint>

namespace wxl::core::gx
{
    enum class Outcome
    {
        not_asked, ///< no [gx] api = vulkan, or the client's adapter asked already
        released,  ///< the device was made, logged and released: the client keeps D3D9
        failed,    ///< no device: why was logged, the client keeps D3D9
    };

    /**
     * @brief The client's graphics device is up (ConsoleDevice.Initialize After). On the stock
     *        client, which has no Vulkan adapter, `[gx] api = vulkan` in @p settings makes the
     *        Vulkan device here, logs it and releases it; the recompiled client asked through its
     *        adapter already.
     * @param why  the reason when it fails, when given
     */
    Outcome DeviceReady(const settings::Settings& settings, char* why = nullptr, uint32_t why_size = 0);
}
