// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The Map seams: what the runtime does when the client places a map doodad and unloads a map,
// on both builds.

#include "wxl/seams/seams.h"

#include "binding/Binding.hpp"
#include "runtime/Core.hpp"
#include "services/TerrainService.hpp"
#include "services/TerrainLayers.hpp"

#include "wxl/events.h"
#include "wxl/host/mod_host.hpp"

#include <cstdint>
#include <cstring>

namespace
{
    /// The def CMap::AllocDoodadDef handed out last, while someone listens: the overload that
    /// called it is placing that doodad, not answering one already placed. Main thread only.
    const void* g_allocated = nullptr;

    /// After of every overload: DoodadSpawn for a def the call allocated.
    void Placed(const void* def, const char* path)
    {
        if (!def || def != g_allocated) return;
        g_allocated = nullptr;

        wxl::host::ModHost* host = wxl::core::Host();
        if (!host || !host->Listening(WXL_EV_DoodadSpawn)) return;

        wxl_doodad_args args{};
        args.doodad = WXL_HANDLE_NONE;
        if (!path) path = "";
        args.path = wxl_str{path, static_cast<uint32_t>(std::strlen(path))};
        wxl::core::binding::doodad_position(def, &args.position);
        host->Raise(WXL_EV_DoodadSpawn, &args);
    }
}

namespace wxl::seam::Map::AllocDoodadDef
{
    void WXL_CALL After(void* result)
    {
        wxl::host::ModHost* host = wxl::core::Host();
        g_allocated = host && host->Listening(WXL_EV_DoodadSpawn) ? result : nullptr;
    }
}

namespace wxl::seam::Map::CreateDoodadDef
{
    void WXL_CALL After(void* result, const char* file_name, void*, const wxl_vec3*)
    {
        Placed(result, file_name);
    }
}

namespace wxl::seam::Map::CreateDoodadDefAt
{
    void WXL_CALL After(void* result, const char* file_name, const wxl_vec3*, float, int32_t)
    {
        Placed(result, file_name);
    }
}

namespace wxl::seam::Map::CreateDoodadDefInMapObj
{
    void WXL_CALL After(void* result, uint32_t, void*, char* name, uint32_t, void*, uint16_t)
    {
        Placed(result, name);
    }
}

namespace wxl::seam::Map::Unload
{
    void WXL_CALL Before()
    {
        // The ground goes with the map: the edits of it are dropped before its chunks are freed.
        wxl_terrain_cleared_args args{};
        wxl::core::services::terrain::MapUnloading(args);
        wxl::core::services::terrain_layers::MapUnloading();
        wxl::host::ModHost* host = wxl::core::Host();
        if (host && host->Listening(WXL_EV_TerrainEditsCleared)) host->Raise(WXL_EV_TerrainEditsCleared, &args);
    }
}
