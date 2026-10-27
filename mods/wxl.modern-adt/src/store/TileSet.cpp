// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The files of each tile beside its root (see TileSet.hpp).

#include "store/TileSet.hpp"

#include "adt/Reader.hpp"

#include <cctype>
#include <string>

namespace wxl::modern_adt
{
    namespace
    {
        /// The most tiles kept: a client holds far fewer at once.
        constexpr size_t kMostTiles = 64;
    }

    std::string TileSet::SplitName(std::string_view root, std::string_view suffix)
    {
        if (root.size() < 4) return {};
        std::string tail(root.substr(root.size() - 4));
        for (char& c : tail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (tail != ".adt") return {};
        return std::string(root.substr(0, root.size() - 4)) + std::string(suffix) + ".adt";
    }

    bool TileSet::IsSplitRoot(const void* root, uint32_t size)
    {
        const adt::Reader  reader(root, size);
        const adt::Chunk   mhdr = reader.Mhdr();
        if (!mhdr || mhdr.size < adt::kSlotMcin + 4) return false;
        const uint8_t* at   = mhdr.data + adt::kSlotMcin;
        const uint32_t mcin = static_cast<uint32_t>(at[0]) | static_cast<uint32_t>(at[1]) << 8 | static_cast<uint32_t>(at[2]) << 16 |
                              static_cast<uint32_t>(at[3]) << 24;
        return mcin == 0 && static_cast<bool>(reader.Find(adt::kMCNK));
    }

    const TileSet::Files& TileSet::Loaded(std::string_view path, int32_t tile_x, int32_t tile_y, const void* root, uint32_t size)
    {
        const uint32_t key = static_cast<uint32_t>(tile_y & 63) * 64u + static_cast<uint32_t>(tile_x & 63);
        tiles_.erase(key);
        if (tiles_.size() >= kMostTiles)
        {
            auto oldest = tiles_.begin();
            for (auto it = tiles_.begin(); it != tiles_.end(); ++it)
                if (it->second.stamp < oldest->second.stamp) oldest = it;
            tiles_.erase(oldest);
        }
        Files& files   = tiles_[key];
        files.root     = root;
        files.rootSize = size;
        files.split    = IsSplitRoot(root, size);
        files.stamp    = ++stamp_;
        if (!read_) return files;
        const auto read = [&](std::string_view suffix, std::vector<uint8_t>& out) {
            const std::string name = SplitName(path, suffix);
            if (!name.empty() && read_(name, out)) ++reads_;
            else out.clear();
        };
        read("_tex0", files.tex0);
        if (files.split)
        {
            read("_obj0", files.obj0);
            read("_obj1", files.obj1);
        }
        files.textureNames = TextureNames(files.tex0);
        return files;
    }

    std::vector<std::string> TileSet::TextureNames(const std::vector<uint8_t>& tex0) const
    {
        std::vector<std::string> names;
        const adt::Reader        reader(tex0.data(), tex0.size());
        for (std::string_view n : reader.TextureNames()) names.emplace_back(n);
        if (!names.empty()) return names;
        const adt::Chunk mdid = reader.Find(adt::Id("MDID"));
        for (uint32_t i = 0; mdid && 4 * i + 4 <= mdid.size; ++i)
        {
            const uint8_t* at = mdid.data + 4 * i;
            const uint32_t id = static_cast<uint32_t>(at[0]) | static_cast<uint32_t>(at[1]) << 8 | static_cast<uint32_t>(at[2]) << 16 |
                                static_cast<uint32_t>(at[3]) << 24;
            std::string name;
            if (!name_ || !id || !name_(id, name)) name = "FileDataID_" + std::to_string(id) + ".blp";
            names.push_back(std::move(name));
        }
        return names;
    }

    void TileSet::ReadMissingByIds(int32_t tile_x, int32_t tile_y, const void* wdt, uint32_t wdtSize)
    {
        if (!name_ || !read_ || tile_x < 0 || tile_y < 0 || tile_x >= 64 || tile_y >= 64) return;
        const auto at = tiles_.find(static_cast<uint32_t>(tile_y) * 64u + static_cast<uint32_t>(tile_x));
        if (at == tiles_.end()) return;
        Files&           files = at->second;
        const adt::Chunk maid  = adt::Reader(wdt, wdtSize).Find(adt::Id("MAID"));
        // MAID: 64 x 64 tiles, row by row, 8 FileDataIDs each: root, obj0, obj1, tex0, lod, map texture, its normals, minimap.
        const size_t entry = (static_cast<size_t>(tile_y) * 64 + static_cast<size_t>(tile_x)) * 32;
        if (!maid || entry + 32 > maid.size) return;
        const auto id = [&](uint32_t slot) {
            const uint8_t* p = maid.data + entry + 4 * slot;
            return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
                   static_cast<uint32_t>(p[3]) << 24;
        };
        const auto read = [&](uint32_t slot, std::vector<uint8_t>& out) {
            std::string name;
            if (out.empty() && id(slot) && name_(id(slot), name) && read_(name, out)) ++reads_;
        };
        const bool hadTex0 = !files.tex0.empty();
        read(3, files.tex0);
        if (files.split)
        {
            read(1, files.obj0);
            read(2, files.obj1);
        }
        if (!hadTex0 && !files.tex0.empty()) files.textureNames = TextureNames(files.tex0);
    }

    const TileSet::Files* TileSet::Of(int32_t tile_x, int32_t tile_y) const
    {
        if (tile_x < 0 || tile_y < 0 || tile_x >= 64 || tile_y >= 64) return nullptr;
        const auto at = tiles_.find(static_cast<uint32_t>(tile_y) * 64u + static_cast<uint32_t>(tile_x));
        return at == tiles_.end() ? nullptr : &at->second;
    }
}
