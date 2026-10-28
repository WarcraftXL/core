// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// A chunk's layers past the client's four, from the split texture file retail writes next to a
// tile (<tile>_tex0.adt: MTEX, MTXP and 256 MCNKs of MCLY and MCAL, up to 8 layers a chunk), through
// wxl.terrain's chunk_fn. The root ADT holds the first four of a chunk's layers (what a client
// without this mod draws); a chunk whose split file has more, and whose first four are the root's
// own textures in the same order, is answered layers 4 to 7: their textures, scales (MTXP or
// MTXF), heights (as height_blend gives them, layers 4 and 5) and alpha maps, decoded from MCAL.
//
// The split file is read when the tile is (TerrainTileLoad). A tile without one has its layers past
// four read from the root ADT itself, whose MCNK may name up to 8 (as Noggit-style map tools write
// them; the runtime cuts the client's count to 4, MCLY keeps them all). A chunk of
// four or fewer layers, or a split chunk whose first four differ from the root's, is drawn as the
// client draws it.

#pragma once

#include "adt/Reader.hpp"
#include "features/HeightBlend.hpp"
#include "store/Store.hpp"

#include "wxl/services/terrain.h"

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace wxl::modern_adt
{
    class ExtraLayers
    {
    public:
        /// @p read fills a file's bytes, false when it is not there; @p blend gives heights (null: none).
        ExtraLayers(Store& store, HeightBlend* blend, std::function<bool(const std::string&, std::vector<uint8_t>&)> read)
            : store_(store), blend_(blend), read_(std::move(read))
        {
        }

        /// The tile at @p path (a root ADT) was read: its split texture file is read too.
        void TileLoaded(std::string_view path, int32_t tile_x, int32_t tile_y);
        /// The tile (x, y) was read with its split texture file @p tex0 (empty: none).
        /// @p names: its textures when it names them by FileDataID (TileSet::TextureNames).
        void TileLoaded(int32_t tile_x, int32_t tile_y, const std::vector<uint8_t>& tex0, const std::vector<std::string>* names = nullptr);

        /// A chunk's layers past its four.
        void Answer(const wxl_terrain_chunk& chunk, wxl_terrain_extra_layers& out);

        /// "x\\y_32_48.adt" -> "x\\y_32_48_tex0.adt"; empty for a name without ".adt".
        static std::string SplitName(std::string_view root);

        uint64_t Files() const { return files_; }
        uint64_t Chunks() const { return chunks_; }
        uint64_t Mismatched() const { return mismatched_; }
        uint64_t FromRoot() const { return fromRoot_; }

    private:
        struct Split
        {
            std::vector<uint8_t>           bytes;
            std::vector<std::string>       names;
            std::vector<adt::TextureInfo>  textures;
            std::vector<adt::SplitChunk>   chunks;
            uint64_t                       stamp = 0;
        };

        Store&                                                         store_;
        HeightBlend*                                                   blend_;
        std::function<bool(const std::string&, std::vector<uint8_t>&)> read_;
        std::map<uint32_t, Split>                                      splits_; ///< by tile: 64 * y + x
        std::vector<std::string>                                       answered_; ///< names handed out, valid for the call
        uint64_t files_ = 0, chunks_ = 0, mismatched_ = 0, stamp_ = 0, fromRoot_ = 0;
    };
}
