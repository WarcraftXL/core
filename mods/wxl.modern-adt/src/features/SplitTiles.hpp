// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// Retail tiles split by file name (Cataclysm to Legion: a root without MCIN, its _tex0, _obj0 and
// _obj1 beside it), through wxl.terrain's tile provider: when the client has read such a root,
// the runtime asks for the set and loads the tile natively, the client's own structures pointing
// into the files. The files are the TileSet's, read when the root was (or now, when the runtime
// asks first).

#pragma once

#include "store/TileSet.hpp"

#include "wxl/services/terrain.h"

namespace wxl::modern_adt
{
    class SplitTiles
    {
    public:
        explicit SplitTiles(TileSet& tiles) : tiles_(tiles) {}

        /// A tile provider's answer for @p tile.
        void Answer(const wxl_terrain_tile& tile, wxl_terrain_tile_files& out);

        uint64_t Tiles() const { return tiles_answered_; }
        uint64_t Missing() const { return missing_; }

    private:
        TileSet& tiles_;
        uint64_t tiles_answered_ = 0, missing_ = 0;
    };
}
