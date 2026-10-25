// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The wxl.engine service: the layout hashes of the engine's structures on this runtime.

#pragma once

#include "wxl/services/engine.h"

namespace wxl::core::services
{
    /// The table, WXL_ENGINE_SERVICE version WXL_ENGINE_SERVICE_VERSION.
    const wxl_engine_api& EngineApi();
}
