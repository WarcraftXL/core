// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The wxl.draw service: simple shapes drawn in the world, on every device.

#pragma once

#include <cstdint>

#include "wxl/services/draw.h"

namespace wxl::gx::shapes
{
    class Ground;
}

namespace wxl::vk::render
{
    class Render;
}

namespace wxl::core::services
{
    /// The table, WXL_DRAW_SERVICE version WXL_DRAW_SERVICE_VERSION.
    const wxl_draw_api& DrawApi();

    namespace draw
    {
        /// The world has drawn its opaque geometry and its liquids (WorldScene.RenderLiquidPass
        /// After): the shapes go onto it, through the Vulkan renderer or the Direct3D 9 device.
        void Render();

        /// A frame is presented (Gx.ScenePresent Before): immediate shapes of a frame that drew no
        /// world are dropped; while shapes are left out past the budget, one line a second says so.
        void Present();

        /// The Vulkan device's renderer goes with everything made on it.
        void DeviceGone();

        /// A mod's shapes go: disabled or unloaded.
        void ForgetMod(uint32_t mod);

        /// Every shape is forgotten and what was made on the device released: the client is
        /// shutting down.
        void Clear();

        /// An interface is coming up: WxlDraw(shape, style) is put in its Lua, which draws a shape
        /// for the next frame of the world (wxl.draw's immediate mode), from tables as wxl.lua's.
        void OnScriptReady();

        /// Tests: the ground shapes lie on @p ground instead of wxl.terrain's (null: wxl.terrain's).
        void UseGround(wxl::gx::shapes::Ground* ground);

        /// Tests: draw through @p render instead of the Vulkan device's (null: the device's).
        void UseVulkan(wxl::vk::render::Render* render);
    }
}
