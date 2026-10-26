// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The wxl.texture service: textures made from an SVG or a PNG, served to the client under paths of
// their own ("Interface\\WXL\\..."), and the function WxlTexture of the game's interface.

#pragma once

#include "wxl/services/texture.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace wxl::core::services
{
    /// The table, WXL_TEXTURE_SERVICE version WXL_TEXTURE_SERVICE_VERSION.
    const wxl_texture_api& TextureApi();

    namespace texture
    {
        /// A script context is up (FrameScript.CreateEvents, before the mods hear of it): the
        /// interface's WxlTexture is put in it.
        void OnScriptReady();

        /**
         * @brief What WxlTexture does once its wrapper has flattened the groups table: the
         *        groups as lines "id\tcolour\twidth\tvisible", empty fields unset; a colour as
         *        "rrggbb", "rrggbbaa" (an optional '#') or "@<decimal 0xAARRGGBB>". A source of
         *        the calling mod's folder (".\\") is refused: an add-on has none.
         */
        wxl_result MakeForInterface(const std::string& source, double width, double height, const std::string& groups,
                                    const std::string& tint, wxl_texture* out);

        /// The client's folder holding Mods, for a harness that has no client: the folder
        /// "Mods\\..." sources are read from is @p mods.
        void UseModsDirectory(const std::filesystem::path& mods);

        /// The budget, for the harness: bytes kept in memory, names known.
        void SetLimits(uint64_t bytes, uint32_t textures);

        /// Everything forgotten and the provider taken back: the client is shutting down, or the
        /// harness starts again.
        void Clear();
    }
}
