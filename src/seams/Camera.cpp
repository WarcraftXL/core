// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The Camera seam: what the runtime does around the world camera's update.

#include "wxl/seams/seams.h"

#include "binding/Binding.hpp"
#include "runtime/Core.hpp"
#include "runtime/Frame.hpp"
#include "services/CameraService.hpp"

#include "wxl/events.h"
#include "wxl/host/mod_host.hpp"

namespace wxl::seam::Camera::UpdateCallback
{
    void WXL_CALL Before(const void* data, void* param)
    {
        (void)data;
        wxl::core::services::camera::BeforeUpdate(param);
    }

    void WXL_CALL After(int32_t result, const void* data, void* param)
    {
        (void)result;
        (void)data;
        // Only the world camera: the one the world frame draws from.
        if (!param || param != wxl::core::binding::camera_world()) return;

        wxl::host::ModHost* host = wxl::core::Host();
        wxl_frame_args      args{};
        if (host && wxl::core::frame::Current(args)) host->Raise(WXL_EV_CameraUpdate, &args);

        // What a handler set just now is written for this frame.
        wxl::core::services::camera::AfterUpdate(param);
    }
}
