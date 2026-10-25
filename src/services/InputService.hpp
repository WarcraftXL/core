// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The wxl.input service: the pointer held at the centre of the game's window for the mods that
// capture it, and the raw motion the Input seam raises as MouseMotion.

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

        /// A mod's capture goes: disabled or unloaded.
        void ForgetMod(uint32_t mod);

        /// Every capture goes and the pointer is free again: the client is shutting down.
        void Clear();
    }
}
