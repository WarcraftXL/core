// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The ProjectTex seams: a texture projected onto the ground, which wxl.selectioncircle skips or
// restyles when it is one of the game's circles.

#include "wxl/seams/seams.h"

#include "services/SelectionCircleService.hpp"

namespace wxl::seam::ProjectTex::Render2d
{
    bool WXL_CALL Gate(const void** box, const void** color, const void**, float*, uint32_t*, uint32_t*, float*)
    {
        return wxl::core::services::selectioncircle::Project(box, color);
    }
}
