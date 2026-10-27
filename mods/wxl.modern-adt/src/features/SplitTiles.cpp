// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// Retail tiles split by file name (see SplitTiles.hpp).

#include "features/SplitTiles.hpp"

#include <string_view>

namespace wxl::modern_adt
{
    void SplitTiles::Answer(const wxl_terrain_tile& tile, wxl_terrain_tile_files& out)
    {
        const TileSet::Files* files = tiles_.Of(tile.tile_x, tile.tile_y);
        if (!files || files->root != tile.file || files->rootSize != tile.file_size)
            files = &tiles_.Loaded(std::string_view(tile.path.data, tile.path.size), tile.tile_x, tile.tile_y, tile.file, tile.file_size);
        const std::vector<uint8_t>* kinds[WXL_TERRAIN_TILE_FILES] = {};
        kinds[WXL_TERRAIN_TILE_TEX0] = &files->tex0;
        kinds[WXL_TERRAIN_TILE_OBJ0] = &files->obj0;
        kinds[WXL_TERRAIN_TILE_OBJ1] = &files->obj1;
        for (uint32_t k = 0; k < WXL_TERRAIN_TILE_FILES; ++k)
            if (!kinds[k]->empty())
            {
                out.data[k] = kinds[k]->data();
                out.size[k] = static_cast<uint32_t>(kinds[k]->size());
            }
        if (files->tex0.empty() || files->obj0.empty()) ++missing_;
        ++tiles_answered_;
    }
}
