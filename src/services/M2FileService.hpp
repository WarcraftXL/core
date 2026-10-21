// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 WarcraftXL
//
// The wxl.m2file service: model files as the client's loader reads them, for the x64 build.

#pragma once

#include <cstdint>

#include "wxl/services/m2file.h"

namespace wxl::core::services
{
    /// The table, WXL_M2FILE_SERVICE version WXL_M2FILE_SERVICE_VERSION.
    const wxl_m2file_api& M2FileApi();

    namespace m2file
    {
        /// Whether any mod watches model files: the loader seams' whole cost when none does.
        bool Watching();

        /// Hands a file at one stage to every watcher, each under the guard. False when one
        /// refused it or faulted on it.
        bool Dispatch(wxl_m2file& file);

        /// Drops every watcher a mod registered, when it is disabled.
        void ForgetMod(uint32_t mod);

        /// Forgets every watcher, on the way out. Memory handed out by alloc_near stays: the
        /// models pointing into it may outlive the mods.
        void Clear();
    }
}
