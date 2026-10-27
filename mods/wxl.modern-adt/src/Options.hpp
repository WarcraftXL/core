// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// modern-adt.ini, in the mod's folder: one section per feature.
//
//   [texture_scale]
//   enabled = 1        ; retail's per-texture scale (MTXP, MTXF) on the maps that carry it
//   source  = auto     ; auto (MTXP when a file has it, else MTXF), mtxp or mtxf
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
        std::vector<std::string> problems; ///< lines not understood
    };

    /// The options a file's text sets.
    Options ParseOptions(std::string_view text);
}
