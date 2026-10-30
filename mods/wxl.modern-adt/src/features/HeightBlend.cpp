// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// Retail's height blend (see HeightBlend.hpp).

#include "features/HeightBlend.hpp"

#include "shader/Ps3.hpp"

#include <cctype>
#include <cstddef>
#include <cstring>

namespace wxl::modern_adt
{
    std::string HeightBlend::HeightName(const std::string& texture)
    {
        if (texture.size() < 4) return {};
        std::string tail = texture.substr(texture.size() - 4);
        for (char& c : tail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (tail != ".blp") return {};
        return texture.substr(0, texture.size() - 4) + "_h" + texture.substr(texture.size() - 4);
    }

    bool HeightBlend::Height(const std::string& texture, const adt::TextureInfo& info, const char** path, float params[4])
    {
        if (info.source != adt::Source::mtxp || (info.flags & 1)) return false;
        if (info.height_scale == 0.0f && info.height_offset == 1.0f) return false; // retail's "no _h"
        const std::string name = HeightName(texture);
        if (name.empty()) return false;
        auto known = known_.find(name);
        if (known == known_.end()) known = known_.emplace(name, exists_ && exists_(name)).first;
        if (!known->second) return false;
        *path     = known->first.c_str();
        params[0] = info.height_scale;
        params[1] = info.height_offset;
        params[2] = 1.0f;
        params[3] = 0.0f;
        return true;
    }

    bool HeightBlend::MapAllows(const void* wdt, uint32_t size)
    {
        if (!requireMapFlag_) return true;
        if (wdt != mapFile_ || size != mapSize_)
        {
            uint32_t flags = 0;
            mapFile_       = wdt;
            mapSize_       = size;
            mapAllows_     = adt::MapFlags(adt::Reader(wdt, size), &flags) && (flags & adt::kMapHeightTexturing);
        }
        return mapAllows_;
    }

    void HeightBlend::Answer(const wxl_terrain_layer& layer, wxl_terrain_layer_look& look)
    {
        const bool mapKnown = layer.struct_size >= offsetof(wxl_terrain_layer, map_file_size) + sizeof layer.map_file_size;
        if (!MapAllows(mapKnown ? layer.map_file : nullptr, mapKnown ? layer.map_file_size : 0)) return;
        if (look.struct_size < offsetof(wxl_terrain_layer_look, params) + sizeof look.params) return; // a runtime before abi 0.1.43
        const Layer* kept = store_.LayerAt(layer.chunk_x, layer.chunk_y, layer.layer);
        const Area&  area = store_.AreaOf(layer.tile_x, layer.tile_y, layer.file, layer.file_size);
        if (!kept || layer.texture >= area.names.size()) return;
        if (Height(area.names[layer.texture], kept->info, &look.extra_texture, look.params)) ++blended_;
    }

    bool HeightBlend::Program(const wxl_gx_shader_code& code, std::vector<uint32_t>& out)
    {
        if (code.stage != 1 || !code.name || std::strncmp(code.name, "Terrain", 7) != 0) return false;
        shader::Found found;
        std::string   why;
        if (!shader::BlendByHeight(code.code, code.size, out, found, why))
        {
            if (why != "fewer than two layers" && why != "not ps_3_0")
            {
                ++refused_;
                lastRefusal_ = std::string(code.name) + ": " + why;
            }
            return false;
        }
        ++programs_;
        return true;
    }
}
