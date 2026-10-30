// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.storage's reload_asset: the client reads a model or a texture it holds again.

#pragma once

#include "wxl/services/storage.h"

namespace wxl::core::services::reload
{
    /**
     * @brief What wxl_storage_api::reload_asset answers: a texture read again in place, a model
     *        dropped from the model cache and the units drawn with it made again.
     */
    wxl_result Reload(const char* name, wxl_asset_reload* out);
}
