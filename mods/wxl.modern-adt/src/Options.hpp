// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// modern-adt.ini, in the mod's folder: one section per feature.
//
//   [texture_scale]
//   enabled = 1        ; retail's per-texture scale (MTXP, MTXF) on the maps that carry it
//   source  = auto     ; auto (MTXP when a file has it, else MTXF), mtxp or mtxf
//
//   [height_blend]
//   enabled = 1        ; retail's height blend (MTXP heightScale and heightOffset, the _h textures)
//   require_wdt_flag = 1 ; only on maps whose WDT says so (MPHD 0x80), as retail; 0 for data made without it
//
//   [extra_layers]
//   enabled = 1        ; up to 8 layers a chunk, from each tile's _tex0.adt or its root (needs height_blend)
//
// A missing file or key keeps the default; a line not understood is logged and left out.

#pragma once

#include "adt/Reader.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace wxl::modern_adt
{
    struct Options
    {
        bool        texture_scale = true;
        adt::Prefer source = adt::Prefer::automatic;
        bool        height_blend = true;
        bool        require_wdt_flag = true;
        bool        extra_layers = true;
        std::vector<std::string> problems; ///< lines not understood
    };

    /// The options a file's text sets.
    Options ParseOptions(std::string_view text);
}
