// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// Retail's per-texture scale (see TextureScale.hpp).

#include "features/TextureScale.hpp"

namespace wxl::modern_adt
{
    void TextureScale::Answer(const wxl_terrain_layer& layer, wxl_terrain_layer_look& look)
    {
        ++asked_;
        const Area& area = store_.AreaOf(layer.tile_x, layer.tile_y, layer.file, layer.file_size);
        Layer       kept;
        kept.texture = layer.texture;
        kept.flags   = layer.flags;
        if (layer.texture < area.textures.size()) kept.info = area.textures[layer.texture];
        store_.Keep(layer.chunk_x, layer.chunk_y, layer.layer, kept);
        if (!kept.info.scale) return; // the client's own look, as it left it
        look.texture_scale = adt::ScaleOf(kept.info.scale);
        ++scaled_;
    }
}
