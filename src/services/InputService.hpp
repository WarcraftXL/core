// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The wxl.input service: the pointer held at the centre of the game's window, or at the point a
// mod chose, for the mods that capture it, and the raw motion the Input seam raises as
// MouseMotion.

#pragma once

#include <cstdint>

#include "wxl/services/input.h"

namespace wxl::core::services
{
    /// The table, WXL_INPUT_SERVICE version WXL_INPUT_SERVICE_VERSION.
    const wxl_input_api& InputApi();

    namespace input
    {
        /// The game's window, from the first message its procedure is handed.
        void NoteWindow(void* window);

        /// The motion a raw input report (WM_INPUT's lparam) carries; false for a report of
        /// another device.
        bool RawMotion(intptr_t report, int32_t& dx, int32_t& dy);

        /// A frame is presented: the pointer is held, or let go, as the mods' captures and the
        /// window's focus say. Main thread.
        void Present();

        /// The game's CGInputControl::SetControlBit: while a mod blocks the mouse look, a look bit is
        /// kept from the call and its press noted. True when the gate took the call (`result` set).
        bool SetControlBit(int32_t* result, void* control, uint32_t* bit, uint32_t time);

        /// The game's CGInputControl::UnsetControlBit: a look bit the gate kept is released here,
        /// with the click's default action when it was short. True when the gate took the call.
        bool UnsetControlBit(int32_t* result, void* control, uint32_t* bit, uint32_t time);

        /// The pixel of a client area `width` x `height` that holds the point (x, y), fractions
        /// from its top-left corner: (0.5, 0.5) is the pixel (width / 2, height / 2).
        void PixelAt(long width, long height, double x, double y, long& px, long& py);

        /// The game's window, once its procedure has seen a message; null before.
        void* Window();

        /// A mod's capture and its block of the mouse look go: disabled or unloaded.
        void ForgetMod(uint32_t mod);

        /// Every capture and every block go and the pointer is free again: the client is shutting down.
        void Clear();
    }
}
