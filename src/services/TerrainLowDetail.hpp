// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The map's far terrain from a WDL of later clients (seam MapLowDetail.Load). The client of 3.3.5a
// reads MVER, then MWMO, MWID and MODF when they follow, then takes MAOF to follow at once; a WDL of
// Legion and later puts MLDD, MLDX, MLMD, MLMX, MSSN, MSSO, MSSF (map objects by FileDataID, sky
// scenes) before MAOF, and MAOE (an ocean mask) between each MARE and its MAHO, so the client would
// read MLDD's bytes as MAOF's offsets and fault. Such a WDL is read by the runtime: MAOF found by
// its id, each area's MARE and MAHO by theirs (wowdev WDL v18), the client's own areas made from
// them as its loop makes them. Its map objects are left out (named by FileDataID). A WDL the
// client reads is left to it.
//
//   Plan   portable: what a WDL holds and whether the client reads it itself
//   Load   the gate's body: the file read through the client's file layer, then the plan applied

#pragma once

#include "wxl/core/init.h"

#include <cstdint>
#include <string>
#include <vector>

namespace wxl::core::services::low_detail
{
    struct Plan
    {
        bool                           clientReads = true; ///< MWMO or MAOF right after MVER: the client's own layout
        uint32_t                       maof = 0;           ///< MAOF's data, from the file's start; 0 for none
        std::vector<wxl_core_area_low> areas;
        uint32_t                       skipped = 0;        ///< MAOF offsets naming no whole MARE
    };

    /// What @p wdl holds, its areas as CMapLowDetail::Load would set them.
    Plan Read(const uint8_t* wdl, uint32_t size);

    /// The gate: true when the runtime loaded the WDL (or found it unreadable and loaded nothing).
    bool Load(void* detail, const char* mapPath, const char* mapName);
}
