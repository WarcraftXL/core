// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The MapChunk seams: a terrain chunk being made and freed, on both builds.

#include "wxl/seams/seams.h"

#include "runtime/Core.hpp"
#include "services/TerrainService.hpp"
#include "services/TerrainTiles.hpp"

#include "wxl/events.h"
#include "wxl/host/mod_host.hpp"

namespace wxl::seam::MapChunk::Purge
{
    void WXL_CALL Before(void* self)
    {
        // The ground changes whether or not someone listens (wxl.draw's drapes follow it).
        wxl_terrain_chunk_state_args args{};
        if (!wxl::core::services::terrain::ChunkUnloading(self, args)) return;
        wxl::host::ModHost* host = wxl::core::Host();
        if (host && host->Listening(WXL_EV_TerrainChunkUnloaded)) host->Raise(WXL_EV_TerrainChunkUnloaded, &args);
    }
}

namespace wxl::seam::MapChunk::ProcessIffChunks
{
    // A chunk of a split tile: pointed at its _tex0 and _obj0 parts.
    void WXL_CALL After(void* self, bool) { wxl::core::services::terrain_tiles::ChunkRead(self); }
}

namespace wxl::seam::MapChunk::CreateRefs
{
    bool WXL_CALL Gate(void** self, void** area, uint32_t**, uint32_t*, uint32_t*)
    {
        return wxl::core::services::terrain_tiles::CreateRefs(*self, *area);
    }
}
