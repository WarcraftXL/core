// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 WarcraftXL
//
// The wxl.gx service: the API the client's device runs on, and its D3D9 device. The batches it
// draws reach mods as the event ModelBatchDrawn (seams/M2.cpp).

#include "services/GxService.hpp"

#include "binding/Binding.hpp"
#include "gx/Vulkan.hpp"
#include "gx/vk/gxfront/Front.hpp"
#include "services/RenderService.hpp"

namespace
{
    void* WXL_CALL D3d9Device(void) { return wxl::core::binding::gx_d3d9_device(); }

    void WXL_CALL InvalidateState(void)
    {
        // Under Vulkan the renderer binds its own state again before its next draw.
        if (wxl::vk::gxfront::Front* front = wxl::core::gx::VulkanFront())
        {
            front->Renderer().Invalidate();
            return;
        }
        wxl::core::binding::gx_invalidate_state();
    }

    uint32_t WXL_CALL Api(void) { return wxl::core::services::render::Backend(); }

    const wxl_gx_api kApi = {sizeof(wxl_gx_api), &D3d9Device, &InvalidateState, &Api};
}

namespace wxl::core::services
{
    const wxl_gx_api& GxApi() { return kApi; }
}
