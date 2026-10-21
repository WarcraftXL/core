// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 WarcraftXL
//
// The wxl.gx service: the client's D3D9 device and the model batches it draws, for the x64 build.

#pragma once

#include <cstdint>

#include "wxl/services/gx.h"

namespace wxl::core::services
{
    /// The table, WXL_GX_SERVICE version WXL_GX_SERVICE_VERSION.
    const wxl_gx_api& GxApi();

    namespace gx
    {
        /// Whether any mod watches model batches: the M2 seam's whole cost when none does.
        bool Watching();

        /// Hands a batch the client has just drawn to every watcher, each under the guard.
        void DispatchModelDraw(const wxl_gx_model_draw& draw);

        /// Drops every watcher a mod registered, when it is disabled.
        void ForgetMod(uint32_t mod);

        /// Forgets everything, on the way out.
        void Clear();
    }
}
