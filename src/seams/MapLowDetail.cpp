// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The MapLowDetail seams: the map's far terrain read from a WDL of later clients, on both builds.

#include "wxl/seams/seams.h"

#include "services/TerrainLowDetail.hpp"

namespace wxl::seam::MapLowDetail::Load
{
    bool WXL_CALL Gate(void** self, const char** map_path, const char** map_name)
    {
        return wxl::core::services::low_detail::Load(*self, *map_path, *map_name);
    }
}
