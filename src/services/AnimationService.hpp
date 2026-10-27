// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The wxl.animation service: animations mods play on units, through the client's own
// CGUnit_C::PlayBaseAnimation, followed at each CGUnit_C::PreAnimate of their unit.

#pragma once

#include <cstdint>
#include <vector>

#include "wxl/events.h"
#include "wxl/services/animation.h"

namespace wxl::core::services
{
    /// The table, WXL_ANIMATION_SERVICE version WXL_ANIMATION_SERVICE_VERSION.
    const wxl_animation_api& AnimationApi();

    namespace animation
    {
        /// The client readies a unit's model for the frame (Unit.PreAnimate After): the play on
        /// it, if any, is followed (ended, played again, handed back); at the active player's,
        /// plays on units gone from view end too.
        void PreAnimate(void* unit, bool activePlayer);

        /// The animations that ended since the last call, for the seam to raise
        /// UnitAnimationEnded with; emptied.
        void TakeEnded(std::vector<wxl_unit_animation_args>& out);

        /// Whether a play is held or an end waits to be raised: the seam's quick test.
        bool Busy();

        /// A mod's animations end: disabled or unloaded.
        void ForgetMod(uint32_t mod);

        /// Every play is forgotten: the client is shutting down.
        void Clear();
    }
}
