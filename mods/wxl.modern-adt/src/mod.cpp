// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.modern-adt: the client reads ADTs of later expansions' data. A reading layer
// (adt/Reader: the top-level chunks, MHDR, MTXF, MTXP), a store of what each area and each texture
// layer carries (store/Store), and features that use it, each with its own section of
// modern-adt.ini:
//
//   texture_scale  retail's per-texture scale (MTXP, MTXF bits 4 to 7): a texture drawn 2^scale
//                  times larger than the client's eight repeats a chunk, through wxl.terrain's
//                  layer provider. Stock maps carry no scale and are drawn as the client draws them.
//
// Main thread only, as wxl.terrain calls its layer provider.

#include "Options.hpp"
#include "features/TextureScale.hpp"
#include "store/Store.hpp"

#include "wxl/sdk.hpp"
#include "wxl/services/terrain.h"

#include <cstddef>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace
{
    constexpr const char* kTag = "modern-adt";

    wxl::Service<wxl_terrain_api> g_terrain(WXL_TERRAIN_SERVICE, WXL_TERRAIN_SERVICE_VERSION);

    class ModernAdtMod final : public wxl::Mod
    {
    public:
        wxl_result OnLoad() override
        {
            options_ = ReadOptions();
            for (const std::string& line : options_.problems) wxl::Log(WXL_LOG_WARN, kTag, line.c_str());
            store_ = std::make_unique<wxl::modern_adt::Store>(options_.source);
            scale_ = std::make_unique<wxl::modern_adt::TextureScale>(*store_);
            if (!options_.texture_scale)
            {
                wxl::Log(WXL_LOG_INFO, kTag, "texture_scale off ([texture_scale] enabled = 0): no feature runs");
                return WXL_OK;
            }
            if (!g_terrain || !wxl::detail::Carries(g_terrain.Get(), &wxl_terrain_api::set_layer_provider))
            {
                wxl::Log(WXL_LOG_WARN, kTag, "wxl.terrain has no layer provider (a runtime before abi 0.1.42): texture_scale does nothing");
                return WXL_OK;
            }
            wxl_terrain_layer_provider provider{};
            provider.struct_size = sizeof provider;
            provider.fn          = &ModernAdtMod::OnLayer;
            provider.user        = this;
            if (g_terrain->set_layer_provider(&provider) != WXL_OK)
            {
                wxl::Log(WXL_LOG_ERROR, kTag, "wxl.terrain refused the layer provider");
                return WXL_E_FAILED;
            }
            char line[160];
            std::snprintf(line, sizeof line, "texture_scale on: retail's per-texture scale from %s, on the maps whose ADTs carry it",
                          options_.source == wxl::modern_adt::adt::Prefer::mtxp   ? "MTXP"
                          : options_.source == wxl::modern_adt::adt::Prefer::mtxf ? "MTXF"
                                                                                   : "MTXP, else MTXF");
            wxl::Log(WXL_LOG_INFO, kTag, line);
            return WXL_OK;
        }

        void OnUnload() override
        {
            if (g_terrain && wxl::detail::Carries(g_terrain.Get(), &wxl_terrain_api::set_layer_provider)) g_terrain->set_layer_provider(nullptr);
            if (scale_)
            {
                char line[160];
                std::snprintf(line, sizeof line, "texture_scale: %llu layer(s) seen, %llu scaled, %zu area(s) read",
                              static_cast<unsigned long long>(scale_->Asked()), static_cast<unsigned long long>(scale_->Scaled()),
                              store_->Areas());
                wxl::Log(WXL_LOG_INFO, kTag, line);
            }
        }

    private:
        static void WXL_CALL OnLayer(void* user, const wxl_terrain_layer* layer, wxl_terrain_layer_look* look)
        {
            auto* self = static_cast<ModernAdtMod*>(user);
            if (layer && look && self->scale_) self->scale_->Answer(*layer, *look);
        }

        static wxl::modern_adt::Options ReadOptions()
        {
            const wxl_host*         host    = wxl::Host();
            const wxl_runtime_info* runtime = host && host->runtime ? host->runtime() : nullptr;
            if (!runtime || runtime->struct_size < offsetof(wxl_runtime_info, mods_directory) + sizeof(wxl_str) ||
                !runtime->mods_directory.size)
                return {};
            const std::string path =
                std::string(runtime->mods_directory.data, runtime->mods_directory.size) + "\\wxl.modern-adt\\modern-adt.ini";
            std::ifstream     in(path, std::ios::binary);
            if (!in) return {};
            std::ostringstream text;
            text << in.rdbuf();
            return wxl::modern_adt::ParseOptions(text.str());
        }

        wxl::modern_adt::Options                       options_;
        std::unique_ptr<wxl::modern_adt::Store>        store_;
        std::unique_ptr<wxl::modern_adt::TextureScale> scale_;
    };
}

WXL_MOD(ModernAdtMod, "wxl.modern-adt", "Modern ADT")
