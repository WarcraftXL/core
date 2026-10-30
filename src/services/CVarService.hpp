// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The wxl.cvar service: console variables mods and the interface register, made as the client's
// own CVars, on both builds.

#pragma once

#include <cstdint>

#include "wxl/services/cvar.h"

namespace wxl::core::services
{
    /// The table, WXL_CVAR_SERVICE version WXL_CVAR_SERVICE_VERSION.
    const wxl_cvar_api& CVarApi();

    namespace cvar
    {
        /// The client is about to read Config.wtf (ConsoleVar.Initialize Before): the
        /// registrations kept waiting are made, so the file's lines reach them.
        void ConfigReading();

        /// Config.wtf is read (ConsoleVar.Initialize After): the latched variables latch from now
        /// on, and every registration is made at once.
        void ConfigRead();

        /// The client deleted its variables of a category, 0x10 the account's or 0x20 the
        /// character's (ConsoleVar.Delete*CVars After): the registered ones of it are made again.
        void CategoryDeleted(uint32_t category);

        /// A script context is up: WxlRegisterCVar is given to its interface.
        void OnContextReady();

        /// A mod's variables are released: disabled or unloaded. They stay, with their values.
        void ForgetMod(uint32_t mod);

        /// The client is shutting down: every hold is let go. The variables and the strings the
        /// console keeps by pointer stay.
        void Clear();

        /// The callback every registered variable carries, as the client calls it before a set:
        /// false refuses the value (a range, a read-only variable set from outside the service).
        bool Allows(void* cvar, const char* previous, const char* value);
    }
}
