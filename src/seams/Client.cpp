// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 WarcraftXL
//
// The Client seam: what the runtime does as the client takes itself apart.

#include "wxl/seams/seams.h"

#include "common/Log.hpp"
#include "runtime/Attachment.hpp"
#include "runtime/Core.hpp"
#include "runtime/Stages.hpp"
#include "services/DataService.hpp"
#include "services/M2FileService.hpp"
#include "services/M2Service.hpp"
#include "services/StorageService.hpp"

#include "wxl/host/mod_host.hpp"

namespace wxl::seam::Client::Destroy
{
    void WXL_CALL Before()
    {
        wxl::host::ModHost* host = wxl::core::Host();
        if (!host) return;

        // The last thing a mod is told, and the only chance it has to put something away. It
        // runs here rather than at DLL_PROCESS_DETACH because by then the loader lock is held,
        // other libraries may already be gone, and nothing a mod did would be safe.
        wxl::core::stages::Reach(WXL_STAGE_SHUTDOWN);

        // Whatever a mod did not give back itself. This has to happen between the stage,
        // where a mod may still want its hooks and its files, and the unload, which frees the
        // code those hooks jump to.
        wxl::core::attachment::ReleaseAll();
        wxl::core::services::storage::CloseEverything();
        wxl::core::services::data::Clear();
        wxl::core::services::m2::Clear();
        wxl::core::services::m2file::Clear();

        host->UnloadAll();
        WLOG_INFO(wxl::core::kTag, "mods unloaded; the client is on its own from here");
    }
}
