// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 WarcraftXL
//
// The M2 seams: the model batches the client draws, handed to wxl.gx's watchers, the bones of
// the models wxl.m2 attached to follow their parent's, and the model files the loader reads,
// handed to wxl.m2file's watchers.

#include "wxl/seams/seams.h"

#include "binding/Binding.hpp"
#include "services/GxService.hpp"
#include "services/M2FileService.hpp"
#include "services/M2Service.hpp"

namespace wxl::seam::M2::DrawBatch
{
    void WXL_CALL After(void* self)
    {
        // Thousands of batches a frame: nothing at all is done while no mod watches them.
        if (!self || !wxl::core::services::gx::Watching()) return;

        wxl_gx_model_draw draw{};
        draw.struct_size = sizeof draw;
        if (!wxl::core::binding::gx_model_draw(self, &draw)) return;
        wxl::core::services::gx::DispatchModelDraw(draw);
    }
}

namespace
{
    /// Poses a model wxl.m2 attached, once the client posed it: one test while none is.
    void PoseAttached(void* self)
    {
        // Every model, every frame, on two threads: one test while no model follows.
        if (!self || !wxl::core::services::m2::Following()) return;
        wxl::core::services::m2::Pose pose{};
        if (!wxl::core::services::m2::PoseOf(self, pose)) return;
        if (pose.kind == wxl::core::services::m2::Pose::OnBone)
            wxl::core::binding::m2_pose_on_bone(self, pose.keyBone, &pose.offset);
        else
            wxl::core::binding::m2_pose_from_parent(self);
    }
}

namespace wxl::seam::M2::AnimateMT
{
    void WXL_CALL After(void* self, const void*, const void*, const void*, float, float) { PoseAttached(self); }
}

namespace wxl::seam::M2::AnimateMTSimple
{
    void WXL_CALL After(void* self, const void*, const void*, const void*, float, float) { PoseAttached(self); }
}


namespace
{
    namespace m2file = wxl::core::services::m2file;

    /// The file as wxl.m2file's watchers see it at `stage`; false when the client has none to show.
    bool Describe(void* file, uint32_t stage, wxl_m2file& out)
    {
        out             = wxl_m2file{};
        out.struct_size = sizeof out;
        if (!file || !wxl::core::binding::m2_file_info(file, &out)) return false;
        out.struct_size = sizeof out;
        out.stage       = stage;
        out.file        = file;
        return true;
    }
}

namespace wxl::seam::M2::LoadSucceededCallback
{
    void WXL_CALL Before(void* param)
    {
        // One test per model file while no mod watches.
        if (!m2file::Watching()) return;
        wxl_m2file file;
        if (!Describe(param, WXL_M2FILE_READ, file)) return;
        const uint32_t read = file.size;
        if (!m2file::Dispatch(file))
            wxl::core::binding::m2_file_set_size(param, 0); // nothing left to parse: the client reports it corrupt
        else if (file.size < read)
            wxl::core::binding::m2_file_set_size(param, file.size);
    }
}

namespace wxl::seam::M2::Initialize
{
    void WXL_CALL Before(void* self)
    {
        if (!m2file::Watching()) return;
        wxl_m2file file;
        if (Describe(self, WXL_M2FILE_PARSED, file)) m2file::Dispatch(file);
    }
}

namespace wxl::seam::M2::InitializeSkinProfile
{
    void WXL_CALL Before(void* self)
    {
        if (!m2file::Watching()) return;
        wxl_m2file file;
        if (Describe(self, WXL_M2FILE_SKIN, file) && file.skin) m2file::Dispatch(file);
    }
}

namespace wxl::seam::M2::FinishLoadingLowPrioritySequence
{
    void WXL_CALL Before(void* self, uint32_t sequence_index, uint8_t* buffer, uint32_t size)
    {
        if (!m2file::Watching() || !buffer) return;
        wxl_m2file file;
        if (!Describe(self, WXL_M2FILE_SEQUENCE, file)) return;
        file.sequence       = buffer;
        file.sequence_size  = size;
        file.sequence_index = sequence_index;
        m2file::Dispatch(file);
    }
}
