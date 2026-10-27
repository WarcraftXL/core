// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// Retail's height blend (wowdev ADT/v18, MTXP): where layers meet, the one whose height texture
// (the texture's _h.blp: its alpha the height) stands higher shows through, weighted by MTXP's
// heightScale and heightOffset. Two parts:
//
//   Program  the client's single-pass terrain pixel programs (ps_3_0 Terrain1/2/3 and the _pcf
//            forms), rewritten by shader::BlendByHeight, through wxl.gx's shader provider
//   Answer   each layer's look through wxl.terrain's layer provider: its _h texture and its
//            numbers (heightScale, heightOffset, 1), for a texture MTXP gives a height and whose
//            _h file exists; nothing otherwise, so the layer blends as the client blends it
//
// A layer with heightScale 0 and heightOffset 1 (retail's "no _h"), MTXF's bit 0 (a cube map), a
// map without MTXP and every stock map are blended as the client does. Paths that draw a layer a
// pass, or without pixel programs, are not rewritten and draw as the client does.

#pragma once

#include "store/Store.hpp"

#include "wxl/services/gx.h"
#include "wxl/services/terrain.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace wxl::modern_adt
{
    class HeightBlend
    {
    public:
        /// @p exists says whether a file is in the client's archives or folders.
        HeightBlend(Store& store, std::function<bool(const std::string&)> exists, bool requireMapFlag = true)
            : store_(store), exists_(std::move(exists)), requireMapFlag_(requireMapFlag)
        {
        }

        /// Whether the map (its WDT's bytes) blends by height: MPHD 0x80, as retail decides it, unless
        /// [height_blend] require_wdt_flag = 0 lets a map made without the flag blend too.
        bool MapAllows(const void* wdt, uint32_t size);

        /// The layer's height texture and numbers, from what the store kept of it (TextureScale::Answer first).
        void Answer(const wxl_terrain_layer& layer, wxl_terrain_layer_look& look);

        /// A texture's height texture and numbers (heightScale, heightOffset, 1, 0): false for one
        /// MTXP gives no height, or whose _h file is missing. @p path stays valid while this lives.
        bool Height(const std::string& texture, const adt::TextureInfo& info, const char** path, float params[4]);

        /// Other bytecode for a terrain pixel program; false for any other program, or one the rewrite does not take.
        bool Program(const wxl_gx_shader_code& code, std::vector<uint32_t>& out);

        /// "tileset\\a\\b.blp" -> "tileset\\a\\b_h.blp"; empty for a name without ".blp".
        static std::string HeightName(const std::string& texture);

        uint64_t Blended() const { return blended_; }
        uint64_t Programs() const { return programs_; }
        uint64_t Refused() const { return refused_; }
        const std::string& LastRefusal() const { return lastRefusal_; }

    private:
        Store&                                    store_;
        std::function<bool(const std::string&)>  exists_;
        bool                                      requireMapFlag_;
        const void*                               mapFile_ = nullptr; ///< the WDT last asked of
        uint32_t                                  mapSize_ = 0;
        bool                                      mapAllows_ = false;
        std::unordered_map<std::string, bool>     known_; ///< _h files looked for
        uint64_t                                  blended_ = 0, programs_ = 0, refused_ = 0;
        std::string                               lastRefusal_;
    };
}
