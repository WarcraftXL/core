// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The World seams: what the runtime does at the functions that load the world, on both builds.

#include "wxl/seams/seams.h"

#include "binding/Binding.hpp"
#include "common/Log.hpp"
#include "runtime/Core.hpp"
#include "services/M2Service.hpp"

#include "wxl/events.h"
#include "wxl/host/mod_host.hpp"

#include <cstdint>

namespace
{
    /// Whether the mods were told the player entered the world and not yet that it left, the map
    /// and place that entry named, and whether a same-map preload's Before raised a leave its
    /// After answers. The map id at a preload's Before is already the destination's (CMap::Load
    /// runs before World::Preload), so the function's own state cannot say what is being left.
    bool     g_inWorld    = false;
    bool     g_preloading = false;
    uint32_t g_mapId      = 0;

    void Enter(wxl::host::ModHost* host, const wxl_vec3& position)
    {
        wxl_world_args args{};
        args.map_id   = static_cast<uint32_t>(wxl::core::binding::map_id());
        args.position = position;
        g_inWorld     = true;
        g_mapId       = args.map_id;
        WLOG_DEBUG(wxl::core::kTag, "world enter: map %u at %.1f %.1f %.1f", args.map_id, args.position.x,
                   args.position.y, args.position.z);
        host->Raise(WXL_EV_WorldEnter, &args);
    }

    void Leave(wxl::host::ModHost* host, const wxl_vec3& position)
    {
        wxl_world_args args{};
        args.map_id   = g_mapId;
        args.position = position;
        g_inWorld     = false;
        WLOG_DEBUG(wxl::core::kTag, "world leave: map %u", args.map_id);
        host->Raise(WXL_EV_WorldLeave, &args);
    }

    /// Where the world is loaded around, or the origin while no map is.
    wxl_vec3 WorldPosition()
    {
        wxl_vec3 where{};
        wxl::core::binding::world_position(&where);
        return where;
    }
}

namespace wxl::seam::World::EnterWorld
{
    void WXL_CALL After()
    {
        if (g_inWorld) return; // a /reload, or an entry already announced
        wxl::host::ModHost* host = wxl::core::Host();
        if (!host) return;

        wxl_vec3 where{};
        const void* player = wxl::core::binding::local_player_object();
        if (!player || !wxl::core::binding::object_position(player, &where)) where = WorldPosition();
        Enter(host, where);
    }
}

namespace wxl::seam::World::LeaveWorld
{
    void WXL_CALL Before()
    {
        if (wxl::core::binding::ui_reloading()) return;
        // The objects and the map go next: the models mods placed in the world leave with the
        // client's own spell effects.
        wxl::core::services::m2::LeaveWorld();
        if (!g_inWorld) return;
        wxl::host::ModHost* host = wxl::core::Host();
        if (!host) return;
        Leave(host, WorldPosition());
    }
}

namespace wxl::seam::World::Preload
{
    void WXL_CALL Before(const wxl_vec3* position, int32_t show_loading)
    {
        g_preloading = false;
        if (!show_loading || !position || !g_inWorld) return;
        wxl::host::ModHost* host = wxl::core::Host();
        if (!host) return;
        g_preloading = true;
        Leave(host, WorldPosition());
    }

    void WXL_CALL After(const wxl_vec3* position, int32_t show_loading)
    {
        if (!g_preloading) return;
        g_preloading = false;
        wxl::host::ModHost* host = wxl::core::Host();
        if (!show_loading || !position || !host) return;
        Enter(host, *position);
    }
}
