// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// FileDataIDs named (8.1 and later), through wxl.terrain's tile provider's name function and for
// the mod's own reading of _tex0 (MDID): from a listfile (`id;path` lines), the map's own
// (World\Maps\<map>\listfile.csv, in the client's archives) before the mod's
// (Mods\wxl.modern-adt\listfile.csv). Each is read when first needed and indexed once: the index
// is kept in the mod's cache folder by the CSV's hash (store/Listfile), so a launch after the
// first reads the CSV only to hash it.

#pragma once

#include "store/Listfile.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wxl::modern_adt
{
    class FileNames
    {
    public:
        /// Fills a file's bytes; false when it is not there.
        using ReadFn = std::function<bool(const std::string&, std::vector<uint8_t>&)>;
        /// Writes a file of the disk; false when it could not.
        using WriteFn = std::function<bool(const std::string&, const std::vector<uint8_t>&)>;

        /// @p archives reads the client's archives (a map's listfile), @p disk and @p write the
        /// disk (the mod's listfile, the cache); @p folder the mod's folder.
        FileNames(std::string folder, ReadFn archives, ReadFn disk, WriteFn write)
            : folder_(std::move(folder)), archives_(std::move(archives)), disk_(std::move(disk)), write_(std::move(write))
        {
        }

        /// The map a tile's path names (World\Maps\<map>\<map>_x_y.adt) is the one names are asked for.
        void MapOf(std::string_view tilePath);

        /// The name of @p id: the map's listfile's, else the mod's; false when neither knows it.
        bool Name(uint32_t id, std::string& out);

        uint64_t Named() const { return named_; }
        uint64_t Unknown() const { return unknown_; }
        uint64_t Indexed() const { return indexed_; } ///< CSVs indexed (not found in the cache)
        /// What the last listfile read said (for the log); empty when nothing was.
        const std::string& Said() const { return said_; }

    private:
        /// The listfile of @p csv (a path; @p inArchives: in the client's archives), loaded once.
        const Listfile* Load(const std::string& csv, bool inArchives);

        std::string                                      folder_;
        ReadFn                                           archives_, disk_;
        WriteFn                                          write_;
        std::string                                      map_; ///< "World\\Maps\\<map>"
        std::map<std::string, std::unique_ptr<Listfile>> loaded_; ///< by CSV path; null: none there
        uint64_t                                         named_ = 0, unknown_ = 0, indexed_ = 0;
        std::string                                      said_;
    };
}
