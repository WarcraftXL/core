// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.terrain's tiles split by file name (abi 0.1.45, set_tile_provider): a root ADT without MCIN,
// as Cataclysm and later write it, loaded natively. The client's own structures point into the
// tile's files as they would into one file; nothing is made again.
//
//   AreaReading   CMapArea::ProcessIffChunks is about to read a tile (seam MapArea.ProcessIffChunks
//                 Before): for a root without MCIN, the tile providers are asked for its _tex0,
//                 _obj0 and _obj1, which are kept (copied) and indexed (split_tile::Index)
//   AreaRead      it has (After): the client set the header, MFBO and the liquid from the root as
//                 for any tile; the area's chunk index, placements, names and texture flags are
//                 pointed into the files, and _tex0's textures loaded by CMapArea::LoadTextures
//   ChunkRead     CMapChunk::ProcessIffChunks has walked a chunk's MCNK (seam MapChunk.
//                 ProcessIffChunks After): a chunk of a split tile is pointed at its _tex0 MCNK's
//                 layers, alpha maps and shadow, its header given its layer count (at most 4),
//                 alpha size and reference counts, its 64 holes folded into 16
//   CreateRefs    CMapChunk::CreateRefs (seam MapChunk.CreateRefs Gate): for such a chunk, the
//                 client's own call is made twice, map objects (_obj0's MCRW) then doodads (MCRD),
//                 the order it places them from one list; for any tile, placements of models the
//                 client cannot load (model_gate: M2 MD21 or past 264, WMO past 17) are left out
//                 the same way, a tile whose models all load left to the client's own call
//   AreaFreeing   CMap::FreeArea (seam Map.FreeArea Before): the files and the index go
//
// Main thread only, as the client reads its tiles.

#pragma once

#include "wxl/services/terrain.h"

#include <cstdint>

namespace wxl::core::services::terrain_tiles
{
    /// The calling mod's tile provider; null takes it off.
    wxl_result SetProvider(uint32_t mod, const wxl_terrain_tile_provider* provider);
    void       ForgetMod(uint32_t mod);

    void AreaReading(void* area);
    void AreaRead(void* area);
    void ChunkRead(void* chunk);
    /// Whether the call was taken (a chunk of a split tile, or one placing a model the client cannot load).
    bool CreateRefs(void* chunk, void* area, const uint32_t* refs, uint32_t doodadRefs, uint32_t mapObjRefs);
    void AreaFreeing(void* area);

    /// The client shuts down.
    void Clear();

    /// What was done, for the log and the harness.
    struct Counters
    {
        uint64_t tiles = 0;         ///< split tiles loaded
        uint64_t chunks = 0;        ///< their chunks made
        uint64_t layersCut = 0;     ///< chunks whose layers stopped before MCLY's end (past 4, or not drawable)
        uint64_t refsLeftOut = 0;   ///< references left out (a placement that does not exist, a FileDataID)
        uint64_t unprovided = 0;    ///< split tiles no provider answered
        uint64_t named = 0;         ///< FileDataIDs given a name (textures, placements)
        uint64_t unnamed = 0;       ///< FileDataIDs no provider named
    };
    const Counters& Count();
    /// Areas held now.
    uint32_t Held();
}
