// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The ConsoleDevice seam: what the runtime does once the graphics device is brought up.

#include "wxl/seams/seams.h"

#include "binding/Binding.hpp"
#include "gx/Memory.hpp"
#include "gx/Pacing.hpp"
#include "gx/Vulkan.hpp"
#include "runtime/Session.hpp"
#include "runtime/Stages.hpp"

namespace wxl::seam::ConsoleDevice::Initialize
{
    void WXL_CALL After(const char*, bool, bool)
    {
        // The function returns only with a device -- each failure ends the process -- but the
        // stage means a device exists, so that is what is asked rather than assumed.
        if (!wxl::core::binding::gx_device_exists()) return;
        wxl::core::gx::DeviceReady(wxl::core::session::CurrentSettings());
        // On Direct3D 9 the same report the Vulkan device gives when it is made, to read beside it.
        if (!wxl::core::gx::VulkanActive()) wxl::core::gx::memory::Report("with the device made");
        // /console gxStats <label>: the frame lines of a test route's step, on every device.
        wxl::core::gx::pacing::RegisterVariable();
        wxl::core::stages::Reach(WXL_STAGE_GRAPHICS);
    }
}
