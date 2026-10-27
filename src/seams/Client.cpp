// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The Client seam: what the runtime does as the client takes itself apart.

#include "wxl/seams/seams.h"

#include "gx/Memory.hpp"

#include "common/Log.hpp"
#include "objects/Objects.hpp"
#include "runtime/Attachment.hpp"
#include "points/Points.hpp"
#include "runtime/Core.hpp"
#include "runtime/Session.hpp"
#include "runtime/Stages.hpp"
#include "services/DataService.hpp"
#include "services/Assets.hpp"
#include "services/CameraService.hpp"
#include "services/ImGuiService.hpp"
#include "services/InputService.hpp"
#include "services/PoseService.hpp"
#include "services/HighlightService.hpp"
#include "services/SelectionCircleService.hpp"
#include "services/TextureService.hpp"
#include "services/CVarService.hpp"
#include "services/TerrainService.hpp"
#include "services/DrawService.hpp"
#include "services/M2Service.hpp"
#include "services/Hashes.hpp"
#include "services/RelayService.hpp"
#include "services/StorageService.hpp"

#include "wxl/host/mod_host.hpp"

namespace wxl::seam::Client::Destroy
{
    void WXL_CALL Before()
    {
        wxl::host::ModHost* host = wxl::core::Host();
        if (!host) return;

        // The graphics memory as the session leaves it, the device still up.
        wxl::core::gx::memory::Report("at exit");

        // The last thing a mod is told, and the only chance it has to put something away. It
        // runs here rather than at DLL_PROCESS_DETACH because by then the loader lock is held,
        // other libraries may already be gone, and nothing a mod did would be safe.
        wxl::core::stages::Reach(WXL_STAGE_SHUTDOWN);

        // Whatever a mod did not give back itself. This has to happen between the stage,
        // where a mod may still want its hooks and its files, and the unload, which frees the
        // code those hooks jump to.
        wxl::core::attachment::ReleaseAll();
        wxl::core::points::ReleaseAll();
        wxl::core::services::storage::CloseEverything();
        wxl::core::services::hashes::Stop();
        wxl::core::services::relay::Clear();
        wxl::core::services::assets::ReleaseAll();
        wxl::core::services::data::Clear();
        wxl::core::services::m2::Clear();
        wxl::core::services::camera::Clear();
        wxl::core::services::input::Clear();
        wxl::core::services::pose::Clear();
        wxl::core::services::imgui::Shutdown();
        wxl::core::services::highlight::Clear();
        wxl::core::services::selectioncircle::Clear();
        wxl::core::services::texture::Clear();
        wxl::core::services::cvar::Clear();
        wxl::core::services::terrain::Clear();
        wxl::core::services::draw::Clear();
        for (wxl::host::ModHost::ModToken token = 1; token <= host->Loaded().size(); ++token)
        {
            wxl::core::objects::Service().DropWatches(token);
            wxl::core::services::storage::RemoveSearchPathsOf(token);
            wxl::core::services::storage::RemoveRedirectsOf(token);
            wxl::core::services::storage::RemoveOverridesOf(token);
        }

        host->UnloadAll();
        WLOG_INFO(wxl::core::kTag, "mods unloaded; the client is on its own from here");
        wxl::core::session::End("clean exit, the client closed");
    }
}
