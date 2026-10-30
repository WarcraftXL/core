// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The Corpse seams: the target's circle under a selected corpse (wxl.selectioncircle).

#include "wxl/seams/seams.h"

#include "services/SelectionCircleService.hpp"

namespace wxl::seam::Corpse::RenderTargetSelection
{
    // A corpse is no unit: its circle has no reaction.
    void WXL_CALL Before(const void* self) { wxl::core::services::selectioncircle::Begin(WXL_SELECTIONCIRCLE_TARGET, self, false); }
    void WXL_CALL After(const void*) { wxl::core::services::selectioncircle::End(); }
}
