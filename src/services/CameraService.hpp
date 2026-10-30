// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The wxl.camera service: the world camera read through the binding, and the view a mod set,
// written over the client's at the end of each update of the camera.

#pragma once

#include <cstdint>

#include "wxl/services/camera.h"

namespace wxl::core::services
{
    /// The table, WXL_CAMERA_SERVICE version WXL_CAMERA_SERVICE_VERSION.
    const wxl_camera_api& CameraApi();

    namespace camera
    {
        /// The camera's update is about to run: the field of view the client had before a mod's
        /// view changed it goes back, so that the client updates its own camera.
        void BeforeUpdate(void* camera);

        /// The world camera's update has run and CameraUpdate was raised: the view a mod holds is
        /// written over the client's.
        void AfterUpdate(void* camera);

        /// A mod's view goes: disabled or unloaded.
        void ForgetMod(uint32_t mod);

        /// Every view goes: the client is shutting down.
        void Clear();
    }
}
