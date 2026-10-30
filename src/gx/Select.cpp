// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// Which graphics API the client creates.

#include "gx/Select.hpp"

#include "wxl/core/gx_vk.h"

namespace wxl::core::gx
{
    ApiChoice SelectApi(int32_t requested, settings::GxApi setting)
    {
        using settings::GxApi;
        if (setting == GxApi::vulkan && requested == WXL_GX_API_D3D9)
            return {WXL_GX_API_VULKAN, "[gx] api = vulkan in WarcraftXL.ini"};
        if (setting == GxApi::vulkan && requested == WXL_GX_API_D3D9EX)
            return {WXL_GX_API_VULKAN, "[gx] api = vulkan in WarcraftXL.ini, which takes a D3D9Ex request too"};
        if (setting == GxApi::d3d9 && requested == WXL_GX_API_VULKAN)
            return {WXL_GX_API_D3D9, "[gx] api = d3d9 in WarcraftXL.ini"};
        return {requested, nullptr};
    }

    const char* ApiName(int32_t api)
    {
        switch (api)
        {
        case WXL_GX_API_OPENGL: return "OpenGL";
        case WXL_GX_API_D3D9: return "D3D9";
        case WXL_GX_API_D3D9EX: return "D3D9Ex";
        case WXL_GX_API_VULKAN: return "Vulkan";
        default: return "another API";
        }
    }
}
