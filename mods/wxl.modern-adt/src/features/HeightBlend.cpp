// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// Retail's height blend (see HeightBlend.hpp).

#include "features/HeightBlend.hpp"

#include "shader/Ps3.hpp"

#include <cctype>
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

    void HeightBlend::Answer(const wxl_terrain_layer& layer, wxl_terrain_layer_look& look)
    {
        if (look.struct_size < sizeof look) return; // a runtime before abi 0.1.43
        const Layer* kept = store_.LayerAt(layer.chunk_x, layer.chunk_y, layer.layer);
        const Area&  area = store_.AreaOf(layer.tile_x, layer.tile_y, layer.file, layer.file_size);
        if (!kept || kept->info.source != adt::Source::mtxp || (kept->info.flags & 1)) return;
        if (kept->info.height_scale == 0.0f && kept->info.height_offset == 1.0f) return; // retail's "no _h"
        if (layer.texture >= area.names.size()) return;
        const std::string name = HeightName(area.names[layer.texture]);
        if (name.empty()) return;
        auto known = known_.find(name);
        if (known == known_.end()) known = known_.emplace(name, exists_ && exists_(name)).first;
        if (!known->second) return;
        look.extra_texture = known->first.c_str();
        look.params[0]     = kept->info.height_scale;
        look.params[1]     = kept->info.height_offset;
        look.params[2]     = 1.0f;
        look.params[3]     = 0.0f;
        ++blended_;
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
