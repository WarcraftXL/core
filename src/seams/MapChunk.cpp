// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The MapChunk seams: a terrain chunk being freed, on both builds.

#include "wxl/seams/seams.h"

#include "runtime/Core.hpp"
#include "services/TerrainService.hpp"

#include "wxl/events.h"
#include "wxl/host/mod_host.hpp"

namespace wxl::seam::MapChunk::Purge
{
    void WXL_CALL Before(void* self)
    {
        wxl::host::ModHost* host = wxl::core::Host();
        if (!host || !host->Listening(WXL_EV_TerrainChunkUnloaded)) return;
        wxl_terrain_chunk_state_args args{};
        if (wxl::core::services::terrain::ChunkUnloading(self, args)) host->Raise(WXL_EV_TerrainChunkUnloaded, &args);
    }
}
