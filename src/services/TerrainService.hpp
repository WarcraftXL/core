// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The wxl.terrain service: the map's ground read and reshaped, on both builds.

#pragma once

#include <cstdint>

#include "wxl/events.h"
#include "wxl/services/terrain.h"

namespace wxl::core::services
{
    /// The table, WXL_TERRAIN_SERVICE version WXL_TERRAIN_SERVICE_VERSION.
    const wxl_terrain_api& TerrainApi();

    namespace terrain
    {
        /// A chunk was built (MapArea.PrepareChunk After): the edits it carries are written into
        /// it and the normals around it made again. Fills TerrainChunkLoaded's args; false for
        /// no chunk.
        bool ChunkLoaded(void* chunk, wxl_terrain_chunk_state_args& args);

        /// A chunk is being freed (MapChunk.Purge Before): its edits stay. Fills
        /// TerrainChunkUnloaded's args; false for no chunk.
        bool ChunkUnloading(void* chunk, wxl_terrain_chunk_state_args& args);

        /// The map is being unloaded (Map.Unload Before): every edit dropped. Fills
        /// TerrainEditsCleared's args.
        void MapUnloading(wxl_terrain_cleared_args& args);

        /// The world starts being drawn (WorldFrame.RenderWorld Before): the ground clutter of a
        /// reshaped chunk is freed only after it.
        void WorldBegin();

        /// The world is drawn (WorldFrame.RenderWorld After): the ground clutter of the chunks
        /// reshaped meanwhile is freed, to grow again on the new ground.
        void WorldEnd();

        /// Whether the map has terrain (loaded, and not a map made only of map objects).
        bool Present();

        /// A loaded chunk's 145 absolute heights, as the client walks on them now, and its holes;
        /// false when it is not loaded.
        bool ChunkGround(int32_t chunk_x, int32_t chunk_y, float heights[145], uint16_t& holes);

        /// Grows each time the ground may have changed: an edit or a reset written, a chunk loaded
        /// or freed, the map unloaded. What is laid on the ground is laid again when it moved.
        uint64_t Generation();

        /// A mod's edits are taken off: disabled or unloaded.
        void ForgetMod(uint32_t mod);

        /// The client is shutting down: everything is forgotten, nothing written.
        void Clear();
    }
}
