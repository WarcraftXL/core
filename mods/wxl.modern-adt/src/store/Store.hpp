// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// What wxl.modern-adt knows of the map, for its features to read:
//
//   per area   each ADT's textures (adt::TextureInfo: scale, heightScale, heightOffset, where they
//              came from), read once from its bytes and read again when the area is loaded again;
//              a root without MCIN has them in its _tex0 (TileSet), read from there
//   per layer  each texture layer the client made: its chunk, its place, its MTEX texture and
//              that texture's info, as wxl.terrain showed it
//
// Main thread only, as wxl.terrain calls the mod.

#pragma once

#include "adt/Reader.hpp"
#include "store/TileSet.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace wxl::modern_adt
{
    struct Area
    {
        const void*                    file = nullptr; ///< the bytes it was read from: another load reads again
        uint32_t                       size = 0;
        std::vector<adt::TextureInfo>  textures;
        std::vector<std::string>       names;          ///< MTEX's, in order
        uint32_t                       scaled = 0;     ///< textures with a scale other than 0
    };

    struct Layer
    {
        uint32_t         texture = 0;
        uint32_t         flags = 0; ///< MCLY's
        adt::TextureInfo info;
    };

    class Store
    {
    public:
        /// @p tiles: where a split tile's _tex0 is found (null: roots only).
        explicit Store(adt::Prefer prefer = adt::Prefer::automatic, const TileSet* tiles = nullptr) : prefer_(prefer), tiles_(tiles) {}

        /// The area of tile (x, y), read from @p file when it is new or was loaded again.
        const Area& AreaOf(int32_t tile_x, int32_t tile_y, const void* file, uint32_t size);
        /// A layer the client made, kept by chunk and place.
        void Keep(int32_t chunk_x, int32_t chunk_y, uint32_t layer, const Layer& value);
        /// The layer at a chunk's place; null when none was seen.
        const Layer* LayerAt(int32_t chunk_x, int32_t chunk_y, uint32_t layer) const;

        size_t Areas() const { return areas_.size(); }
        size_t Layers() const { return layers_.size(); }
        uint64_t Reads() const { return reads_; }

    private:
        adt::Prefer                                prefer_;
        const TileSet*                             tiles_;
        std::unordered_map<uint32_t, Area>         areas_;  ///< by tile: 64 * y + x
        std::unordered_map<uint64_t, Layer>        layers_; ///< by chunk and place
        uint64_t                                   reads_ = 0;
    };
}
