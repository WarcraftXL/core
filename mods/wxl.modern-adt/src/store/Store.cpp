// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// What wxl.modern-adt knows of the map (see Store.hpp).

#include "store/Store.hpp"

namespace wxl::modern_adt
{
    const Area& Store::AreaOf(int32_t tile_x, int32_t tile_y, const void* file, uint32_t size)
    {
        Area& area = areas_[static_cast<uint32_t>(tile_y) * 64u + static_cast<uint32_t>(tile_x)];
        if (area.file != file || area.size != size)
        {
            area.file     = file;
            area.size     = size;
            // A split tile's textures are its _tex0's, which its layers' MTEX indices name.
            const TileSet::Files* set = tiles_ ? tiles_->Of(tile_x, tile_y) : nullptr;
            const bool            split = set && set->root == file && set->split && !set->tex0.empty();
            const adt::Reader     reader(split ? static_cast<const void*>(set->tex0.data()) : file,
                                         split ? set->tex0.size() : size);
            area.textures = adt::ReadTextures(reader, prefer_);
            area.names.clear();
            for (std::string_view name : reader.TextureNames()) area.names.emplace_back(name);
            area.scaled   = 0;
            for (const adt::TextureInfo& t : area.textures) area.scaled += t.scale ? 1u : 0u;
            ++reads_;
        }
        return area;
    }

    void Store::Keep(int32_t chunk_x, int32_t chunk_y, uint32_t layer, const Layer& value)
    {
        if (chunk_x < 0 || chunk_y < 0 || layer > 3) return;
        layers_[(static_cast<uint64_t>(chunk_y) * 1024u + static_cast<uint64_t>(chunk_x)) * 4u + layer] = value;
    }

    const Layer* Store::LayerAt(int32_t chunk_x, int32_t chunk_y, uint32_t layer) const
    {
        if (chunk_x < 0 || chunk_y < 0 || layer > 3) return nullptr;
        const auto at = layers_.find((static_cast<uint64_t>(chunk_y) * 1024u + static_cast<uint64_t>(chunk_x)) * 4u + layer);
        return at == layers_.end() ? nullptr : &at->second;
    }
}
