// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The wxl.pose service: unit heads turned by mods, applied after the game readies each unit's
// model for the frame.

#pragma once

#include <cstdint>

#include "wxl/services/pose.h"

namespace wxl::core::services
{
    /// The table, WXL_POSE_SERVICE version WXL_POSE_SERVICE_VERSION.
    const wxl_pose_api& PoseApi();

    namespace pose
    {
        /// The game readied a unit's model (CGUnit_C::PreAnimate): a head turned by a mod moves
        /// on and is set on the head bone.
        void PreAnimate(void* unit);

        /// A mod's heads are released: disabled or unloaded.
        void ForgetMod(uint32_t mod);

        /// Every head is forgotten: the client is shutting down.
        void Clear();
    }
}
