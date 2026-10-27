// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// A chunk's layers past the client's four (see ExtraLayers.hpp).

#include "features/ExtraLayers.hpp"

#include <cctype>
#include <cstring>

namespace wxl::modern_adt
{
    namespace
    {
        /// The most split files kept: a client holds far fewer tiles at once.
        constexpr size_t kMostSplits = 64;

        bool SameName(std::string_view a, std::string_view b)
        {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i)
            {
                const char x = a[i] == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
                const char y = b[i] == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
                if (x != y) return false;
            }
            return true;
        }
    }

    std::string ExtraLayers::SplitName(std::string_view root)
    {
        if (root.size() < 4 || !SameName(root.substr(root.size() - 4), ".adt")) return {};
        return std::string(root.substr(0, root.size() - 4)) + "_tex0.adt";
    }

    void ExtraLayers::TileLoaded(std::string_view path, int32_t tile_x, int32_t tile_y)
    {
        if (tile_x < 0 || tile_y < 0 || tile_x >= 64 || tile_y >= 64) return;
        const uint32_t key = static_cast<uint32_t>(tile_y) * 64u + static_cast<uint32_t>(tile_x);
        splits_.erase(key);
        const std::string name = SplitName(path);
        Split             split;
        if (name.empty() || !read_ || !read_(name, split.bytes)) return;
        const adt::Reader reader(split.bytes.data(), split.bytes.size());
        for (std::string_view n : reader.TextureNames()) split.names.emplace_back(n);
        split.textures = adt::ReadTextures(reader, adt::Prefer::automatic);
        split.chunks   = adt::ReadSplitChunks(reader);
        split.stamp    = ++stamp_;
        ++files_;
        if (splits_.size() >= kMostSplits)
        {
            auto oldest = splits_.begin();
            for (auto it = splits_.begin(); it != splits_.end(); ++it)
                if (it->second.stamp < oldest->second.stamp) oldest = it;
            splits_.erase(oldest);
        }
        splits_.emplace(key, std::move(split));
    }

    void ExtraLayers::Answer(const wxl_terrain_chunk& chunk, wxl_terrain_extra_layers& out)
    {
        answered_.clear();
        if (chunk.layers != 4 || chunk.chunk_x < 0 || chunk.chunk_y < 0) return;
        const auto at = splits_.find(static_cast<uint32_t>(chunk.tile_y) * 64u + static_cast<uint32_t>(chunk.tile_x));
        if (at == splits_.end()) return;
        const Split&   split = at->second;
        const uint32_t index = static_cast<uint32_t>(chunk.chunk_x % 16) + 16u * static_cast<uint32_t>(chunk.chunk_y % 16);
        if (index >= split.chunks.size() || split.chunks[index].layers.size() <= 4) return;
        const adt::SplitChunk& c = split.chunks[index];
        // Its first four are the root's: the client draws them, the rest go over them.
        const Area& area = store_.AreaOf(chunk.tile_x, chunk.tile_y, chunk.file, chunk.file_size);
        for (uint32_t k = 0; k < 4; ++k)
        {
            const uint32_t mine = c.layers[k].texture, root = chunk.texture[k];
            if (mine >= split.names.size() || root >= area.names.size() || !SameName(split.names[mine], area.names[root]))
            {
                ++mismatched_;
                return;
            }
        }
        const uint32_t count = static_cast<uint32_t>(std::min<size_t>(c.layers.size(), 4 + WXL_TERRAIN_EXTRA_LAYERS) - 4);
        answered_.reserve(2 * count);
        uint8_t  alpha[adt::kAlphaSide * adt::kAlphaSide];
        uint32_t mapFlags = 0;
        const int big = adt::MapFlags(adt::Reader(chunk.map_file, chunk.map_file_size), &mapFlags)
                            ? ((mapFlags & (adt::kMapBigAlpha | adt::kMapHeightTexturing)) ? 1 : 0)
                            : -1;
        for (uint32_t k = 0; k < count; ++k)
        {
            const adt::LayerEntry& l = c.layers[4 + k];
            if (l.texture >= split.names.size()) return;
            const adt::TextureInfo info = l.texture < split.textures.size() ? split.textures[l.texture] : adt::TextureInfo{};
            wxl_terrain_extra_layer& e  = out.layer[k];
            answered_.push_back(split.names[l.texture]);
            e.texture       = answered_.back().c_str();
            e.flags         = l.flags;
            e.texture_scale = adt::ScaleOf(info.scale);
            if (blend_ && k < 2 && blend_->MapAllows(chunk.map_file, chunk.map_file_size))
                blend_->Height(split.names[l.texture], info, &e.extra_texture, e.params);
            if (!adt::DecodeAlpha(c, 4 + k, alpha, big)) std::memset(alpha, 0, sizeof alpha);
            for (uint32_t t = 0; t < adt::kAlphaSide * adt::kAlphaSide; ++t) out.alpha[t * 4 + k] = alpha[t];
        }
        out.count = count;
        ++chunks_;
    }
}
