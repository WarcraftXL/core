// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 WarcraftXL
//
// The M2 seams: the model batches the client draws (ModelBatchDrawn), and the bones of the
// models wxl.m2 hung to follow their parent's, and the bones wxl.m2's watchers move (live physics).

#include "wxl/seams/seams.h"

#include "binding/Binding.hpp"
#include "runtime/Core.hpp"
#include "services/M2Service.hpp"

#include "wxl/events.h"
#include "wxl/host/mod_host.hpp"

namespace wxl::seam::M2::DrawBatch
{
    void WXL_CALL After(void* self)
    {
        // Thousands of batches a frame: one flag is read while no mod listens.
        if (!wxl::seam::Listening(WXL_EV_ModelBatchDrawn)) return;
        wxl::host::ModHost* host = wxl::core::Host();
        if (!self || !host || !host->Listening(WXL_EV_ModelBatchDrawn)) return;

        wxl_model_batch_args args{};
        if (!wxl::core::binding::gx_model_draw(self, &args)) return;
        host->Raise(WXL_EV_ModelBatchDrawn, &args);
    }
}

namespace wxl::seam::M2::AnimateMT
{
    void WXL_CALL After(void* self, const void*, const void*, const void*, float, float)
    {
        wxl::core::services::m2::PoseAttached(self);
        // Then wxl.m2's bone watchers (live physics), on the pose the model ends up with. One
        // flag is read while no mod watches.
        if (self && wxl::core::services::m2::WatchingBones()) wxl::core::services::m2::DispatchBones(self);
    }
}

namespace wxl::seam::M2::AnimateMTSimple
{
    void WXL_CALL After(void* self, const void*, const void*, const void*, float, float)
    {
        wxl::core::services::m2::PoseAttached(self);
    }
}
