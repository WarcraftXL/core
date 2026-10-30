// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 WarcraftXL
//
// The wxl.gx service: the API the client's device runs on, and its Direct3D 9 device.

#pragma once

#include "wxl/services/gx.h"

namespace wxl::core::services
{
    /// The table, WXL_GX_SERVICE version WXL_GX_SERVICE_VERSION.
    const wxl_gx_api& GxApi();
}
