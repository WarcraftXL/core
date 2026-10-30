// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The files of each tile the client loads, beside its root ADT: <tile>_tex0.adt (textures, layers,
// alpha maps), and for a root without MCIN (a tile split by file name, Cataclysm and later) its
// <tile>_obj0.adt and <tile>_obj1.adt. Read when the root is (TerrainTileLoad), or when first
// asked; kept for the 64 tiles read last, which is more than a client holds at once. What the
// features share: the tile provider hands them to the runtime, the texture store and the extra
// layers read _tex0 from here. A _tex0 of 8.1 and later names its textures by FileDataID (MDID):
// their names come from the namer (a listfile), "FileDataID_<id>.blp" for one it does not know, as
// the runtime names them. A map whose WDT lists its tiles' files by FileDataID (MAID) has a file
// missing under its usual name read under the name the namer gives its id.
//
// Main thread only, as wxl.terrain calls the mod.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace wxl::modern_adt
{
    class TileSet
    {
    public:
        /// Fills a file's bytes; false when it is not there.
        using ReadFn = std::function<bool(const std::string&, std::vector<uint8_t>&)>;
        /// The name of a FileDataID; false when unknown.
        using NameFn = std::function<bool(uint32_t, std::string&)>;

        struct Files
        {
            const void*          root = nullptr; ///< the root's bytes the files were read for
            uint32_t             rootSize = 0;
            bool                 split = false;  ///< the root has no MCIN
            std::vector<uint8_t> tex0, obj0, obj1;
            std::vector<std::string> textureNames; ///< _tex0's MTEX, or its MDID's ids named
            uint64_t             stamp = 0;
        };

        explicit TileSet(ReadFn read, NameFn name = nullptr) : read_(std::move(read)), name_(std::move(name)) {}

        /// The files of tile (x, y) a WDT's MAID names (root, obj0, obj1, tex0) that were not found
        /// under their usual names, read under the names their ids are given. @p wdt: the map's WDT.
        void ReadMissingByIds(int32_t tile_x, int32_t tile_y, const void* wdt, uint32_t wdtSize);

        /// The textures a _tex0 names (MTEX, or MDID's ids named).
        std::vector<std::string> TextureNames(const std::vector<uint8_t>& tex0) const;

        /// The tile at @p path (a root ADT, @p root its bytes) was read: its other files are read.
        const Files& Loaded(std::string_view path, int32_t tile_x, int32_t tile_y, const void* root, uint32_t size);

        /// The files read for tile (x, y); null when none were.
        const Files* Of(int32_t tile_x, int32_t tile_y) const;

        /// "x\\y_32_48.adt" -> "x\\y_32_48<suffix>.adt"; empty for a name without ".adt".
        static std::string SplitName(std::string_view root, std::string_view suffix);

        /// Whether a root ADT has no MCIN: MHDR's MCIN slot is 0 and MCNKs follow (a split tile).
        static bool IsSplitRoot(const void* root, uint32_t size);

        uint64_t Reads() const { return reads_; }

    private:
        ReadFn                    read_;
        NameFn                    name_;
        std::map<uint32_t, Files> tiles_; ///< by tile: 64 * y + x
        uint64_t                  stamp_ = 0, reads_ = 0;
    };
}
