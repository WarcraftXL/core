// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// Retail's per-texture scale: each layer drawn 2^scale times larger than the client's own tiling,
// from its texture's MTXP or MTXF flags (bits 4 to 7). A texture without them, and every texture
// of a stock map, is answered 1: drawn as the client draws it.

#pragma once

#include "store/Store.hpp"

#include "wxl/services/terrain.h"

namespace wxl::modern_adt
{
    class TextureScale
    {
    public:
        explicit TextureScale(Store& store) : store_(store) {}

        /// The layer's look, from what the store knows of its area.
        void Answer(const wxl_terrain_layer& layer, wxl_terrain_layer_look& look);

        uint64_t Asked() const { return asked_; }
        uint64_t Scaled() const { return scaled_; }

    private:
        Store&   store_;
        uint64_t asked_ = 0, scaled_ = 0;
    };
}
