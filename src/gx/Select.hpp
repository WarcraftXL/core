// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// Which graphics API the client creates: WarcraftXL.ini's [gx] api over the client's own request.

#pragma once

#include "runtime/Settings.hpp"

#include <cstdint>

namespace wxl::core::gx
{
    struct ApiChoice
    {
        int32_t     api;          ///< the client's EGxApi value to create (WXL_GX_API_*)
        const char* why;          ///< null when the request stands, else what changed it
    };

    /**
     * @brief The API for @p requested (WXL_GX_API_*): `[gx] api = vulkan` takes a D3D9 or D3D9Ex
     *        request to Vulkan, `[gx] api = d3d9` a Vulkan one to D3D9; OpenGL and any request
     *        without the line stand.
     */
    ApiChoice SelectApi(int32_t requested, settings::GxApi setting);

    /// "D3D9", "D3D9Ex", "OpenGL", "Vulkan", or "api <n>".
    const char* ApiName(int32_t api);
}
