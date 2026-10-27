// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The MapArea seams: what the runtime does when a terrain chunk is built, on both builds.

#include "wxl/seams/seams.h"

#include <cstring>

#include "services/Assets.hpp"
#include "services/TerrainService.hpp"

#include "binding/Binding.hpp"
#include "runtime/Core.hpp"

#include "wxl/events.h"
#include "wxl/host/mod_host.hpp"

#include <cstdint>

namespace wxl::seam::MapArea::PrepareChunk
{
    void WXL_CALL Before(void* self, int32_t chunk_x, int32_t chunk_y)
    {
        wxl::host::ModHost* host = wxl::core::Host();
        if (!host || !self || !host->Listening(WXL_EV_TerrainChunkBuild)) return;

        wxl_terrain_chunk_args args{};
        args.map_id  = static_cast<uint32_t>(wxl::core::binding::map_id());
        args.tile_x  = wxl::core::binding::map_area_tile_x(self);
        args.tile_y  = wxl::core::binding::map_area_tile_y(self);
        args.chunk_x = chunk_x;
        args.chunk_y = chunk_y;
        host->Raise(WXL_EV_TerrainChunkBuild, &args);
    }

    void WXL_CALL After(void* result, void*, int32_t, int32_t)
    {
        // The chunk is built and not drawn yet: the edits it carries go back into it, whether or
        // not someone listens.
        wxl_terrain_chunk_state_args args{};
        if (!wxl::core::services::terrain::ChunkLoaded(result, args)) return;
        wxl::host::ModHost* host = wxl::core::Host();
        if (host && host->Listening(WXL_EV_TerrainChunkLoaded)) host->Raise(WXL_EV_TerrainChunkLoaded, &args);
    }
}

namespace wxl::seam::MapArea::AsyncCallback
{
    void WXL_CALL Before(void* param)
    {
        wxl::host::ModHost* host = wxl::core::Host();
        if (!host || !param || !host->Listening(WXL_EV_TerrainTileLoad)) return;

        int32_t     x = 0, y = 0;
        void*       file = nullptr;
        uint32_t    size = 0;
        const void* data = wxl::core::binding::map_area_bytes(param, &x, &y, &file, &size);
        if (!data) return;

        // The name the tile was opened by; one the runtime's file layer opened carries its own
        // prefix, which is not the client's name.
        char        name[260] = {};
        const char* path      = name;
        if (file && wxl::core::binding::file_name(file, name, sizeof name))
        {
            const char* inner = nullptr;
            if (wxl::core::services::assets::IsLayerPath(name, nullptr, &inner)) path = inner;
        }

        wxl_terrain_tile_args args{};
        args.path   = wxl_str{path, static_cast<uint32_t>(std::strlen(path))};
        args.tile_x = x;
        args.tile_y = y;
        args.data   = data;
        args.size   = size;
        host->Raise(WXL_EV_TerrainTileLoad, &args);
    }
}
