// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The Gx seams: what the runtime does at the present, and as the graphics device goes and comes back.

#include "wxl/seams/seams.h"

#include <cstring>

#include "binding/Binding.hpp"
#include "profile/Profile.hpp"
#include "runtime/Core.hpp"
#include "runtime/Frame.hpp"
#include "services/FootprintService.hpp"
#include "services/InputService.hpp"

#include "wxl/events.h"
#include "wxl/host/mod_host.hpp"

#include <cstdint>

namespace
{
    /// The size the device renders at now. Zero when there is no device to ask.
    wxl_swapchain_args Size()
    {
        wxl_swapchain_args args{};
        wxl::core::binding::gx_window_size(&args.width, &args.height);
        return args;
    }
}

namespace wxl::seam::Gx::ScenePresent
{
    void WXL_CALL Before()
    {
        // Every present ends a frame on the profiler's timeline, the loading screen's included.
        wxl::core::profile::Present();

        // A pointer a mod captured is held at the centre, and hidden, before the frame shows.
        wxl::core::services::input::Present();

        // Only the frame Scrn.OnPaint opened ends here. The loading screen presents frames of
        // its own, and a paint with nothing to draw into presents anyway; neither opened one.
        wxl::host::ModHost* host = wxl::core::Host();

        // The address space is looked at here, about once a second, while a mod watches it: the
        // client presents through the loading screen too, which is when memory runs short.
        wxl_memory_pressure_args pressure{};
        if (host && wxl::core::services::footprint::PressureDue(&pressure))
            host->Raise(WXL_EV_MemoryPressure, &pressure);

        wxl_frame_args args{};
        if (!wxl::core::frame::End(args)) return;
        if (!host) return;
        host->Raise(WXL_EV_FrameEnd, &args);
    }
}

namespace wxl::seam::Gx::ReleaseD3dResources
{
    void WXL_CALL Before(void* self, int32_t)
    {
        wxl::host::ModHost* host = wxl::core::Host();
        if (!host || !self) return;

        // Read before the release: this is the size of what is about to go away.
        const wxl_swapchain_args args = Size();
        host->Raise(WXL_EV_SwapchainLost, &args);
    }
}

namespace wxl::seam::Gx::NotifyOnDeviceRestored
{
    void WXL_CALL Before(void* self)
    {
        wxl::host::ModHost* host = wxl::core::Host();
        if (!host || !self) return;

        // Read after the reset, which is what reaching this function means: the new size.
        const wxl_swapchain_args args = Size();
        host->Raise(WXL_EV_SwapchainRestored, &args);
    }
}

namespace wxl::seam::Gx::IShaderLoad
{
    void WXL_CALL Before(void*, void*, uint32_t target, const char* path, const char* name, uint32_t permutations)
    {
        wxl::host::ModHost* host = wxl::core::Host();
        if (!host || !host->Listening(WXL_EV_ShaderLoad)) return;
        if (!path) path = "";
        if (!name) name = "";

        wxl_shader_load_args args{};
        args.stage        = target;
        args.path         = wxl_str{path, static_cast<uint32_t>(strnlen(path, 260))};
        args.name         = wxl_str{name, static_cast<uint32_t>(strnlen(name, 260))};
        args.permutations = permutations;
        host->Raise(WXL_EV_ShaderLoad, &args);
    }
}
