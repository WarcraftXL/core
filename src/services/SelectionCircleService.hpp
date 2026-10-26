// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The wxl.selectioncircle service: the game's circles under its target, its pet's target and the
// move marker, hidden or drawn in another texture, colour or size.

#pragma once

#include <cstdint>

#include "wxl/services/selectioncircle.h"

namespace wxl::core::services
{
    /// The table, WXL_SELECTIONCIRCLE_SERVICE version WXL_SELECTIONCIRCLE_SERVICE_VERSION.
    const wxl_selectioncircle_api& SelectionCircleApi();

    namespace selectioncircle
    {
        /// A circle starts (a drawer's Before): its kind (a WXL_SELECTIONCIRCLE_* bit), the object
        /// it is drawn for, and whether that object is a unit whose reaction is read. The look
        /// that applies is chosen, and a mod's texture put in the game's place until End.
        void Begin(uint32_t kind, const void* object, bool unit);

        /// The circle is drawn (the drawer's After): the game's texture goes back.
        void End();

        /// A projection of the bound texture (ProjectTex.Render2d's gate), by the addresses of its
        /// box and colour arguments. Outside a circle, nothing. Answers true when the circle is
        /// hidden: the projection is skipped. A restyled circle has its arguments pointed at the
        /// service's own box and colour.
        bool Project(const void** box, const void** color);

        /// The world pass is drawn (WorldFrame.RenderWorld After): the frame's counts are kept
        /// for drawn() and hidden().
        void WorldEnd();

        /// A mod's changes are given back: disabled or unloaded.
        void ForgetMod(uint32_t mod);

        /// Every change is forgotten and the mods' textures let go: the client is shutting down.
        void Clear();
    }
}
