// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The wxl.render and wxl.vk services: a mod's images, buffers, pipelines and passes on the runtime's
// Vulkan renderer (gx/vk/render/), and the device itself, raw; the points of the frame where mods
// draw (RenderPoint), raised on every device.

#pragma once

#include <cstdint>
#include <string_view>

#include "wxl/events.h"
#include "wxl/services/render.h"
#include "wxl/services/vk.h"

namespace wxl::core::services
{
    /// The tables, WXL_RENDER_SERVICE and WXL_VK_SERVICE at their versions.
    const wxl_render_api& RenderApi();
    const wxl_vk_api&     VkApi();

    namespace render
    {
        /// What wxl.gx's api answers: the wxl_gx_backend the client draws with now.
        uint32_t Backend();

        /// Whether a mod listens to ModelBatchDrawn on a device that hands it batches to draw again.
        bool WantsBatches();

        /// On the Vulkan renderer: the batch just drawn into @p args (wxl.render's draw of it) and
        /// where it was drawn (D3D9's viewport); nothing on another device.
        void FillBatch(wxl_model_batch_args& args);

        /**
         * @brief A point of the frame is reached: true, with its @p args filled and the point open,
         *        when RenderPoint is to be raised (a mod listens, inside a frame the client paints).
         *        The seam body raises it, then calls PointEnd. Inside a point wxl.render's passes and
         *        draws are taken.
         */
        bool PointBegin(wxl_render_point point, wxl_render_point_args& args);
        /// The point is over: a pass a mod left open is closed, its GPU zones too.
        void PointEnd();

        /// wxl.profile's GPU zones, inside a point.
        wxl_result GpuZoneBegin(std::string_view name);
        void       GpuZoneEnd();

        /// A mod goes: its images, buffers and pipelines are given back, its shaders restored.
        void ForgetMod(uint32_t mod);

        /// The Vulkan renderer goes with everything made on it: every handle is forgotten.
        void DeviceGone();
    }
}
